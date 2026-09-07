#include "SpeechImporter.h"

#include "SpeechForge.h"

#include "AssetToolsModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetImportTask.h"
#include "Factories/SoundFactory.h"
#include "Sound/SoundWave.h"
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "FileHelpers.h"

FString FSpeechImporter::SanitizeAssetName(const FString& In)
{
	FString Out;
	Out.Reserve(In.Len());

	for (const TCHAR Char : In)
	{
		if (FChar::IsAlnum(Char) || Char == TEXT('_'))
		{
			Out.AppendChar(Char);
		}
		else if (Char == TEXT(' ') || Char == TEXT('-') || Char == TEXT('.'))
		{
			Out.AppendChar(TEXT('_'));
		}
	}

	return Out.IsEmpty() ? TEXT("Speech") : Out;
}

FString FSpeechImporter::HashFile(const FString& AbsolutePath)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *AbsolutePath))
	{
		return FString();
	}

	FSHA1 Sha;
	Sha.Update(Bytes.GetData(), Bytes.Num());
	Sha.Final();

	uint8 Digest[FSHA1::DigestSize];
	Sha.GetHash(Digest);

	return BytesToHex(Digest, FSHA1::DigestSize);
}

float FSpeechImporter::ReadWavDuration(const FString& AbsolutePath)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *AbsolutePath) || Bytes.Num() < 44)
	{
		return 0.f;
	}

	auto Tag = [&Bytes](int32 Offset)
	{
		return (Offset + 4 <= Bytes.Num())
			? FString(4, reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Offset))
			: FString();
	};
	auto Read32 = [&Bytes](int32 Offset) -> uint32
	{
		uint32 Value = 0;
		FMemory::Memcpy(&Value, Bytes.GetData() + Offset, sizeof(uint32));
		return Value;
	};

	if (Tag(0) != TEXT("RIFF") || Tag(8) != TEXT("WAVE"))
	{
		return 0.f;
	}

	uint32 BytesPerSecond = 0;
	uint32 DataBytes = 0;

	// Chunks are walked rather than indexed: anything but the simplest encoder writes LIST or fact
	// chunks ahead of the audio, and a fixed 44-byte assumption reads those as samples.
	for (int32 Offset = 12; Offset + 8 <= Bytes.Num(); )
	{
		const FString ChunkId = Tag(Offset);
		const uint32 ChunkSize = Read32(Offset + 4);

		if (ChunkId == TEXT("fmt ") && Offset + 20 <= Bytes.Num())
		{
			BytesPerSecond = Read32(Offset + 16);
		}
		else if (ChunkId == TEXT("data"))
		{
			DataBytes = ChunkSize;
			break;
		}

		// Chunks pad to even boundaries, and an odd size that is not rounded walks the reader one
		// byte out of step for the rest of the file.
		Offset += 8 + ChunkSize + (ChunkSize & 1);
	}

	return (BytesPerSecond > 0 && DataBytes > 0)
		? static_cast<float>(DataBytes) / static_cast<float>(BytesPerSecond)
		: 0.f;
}

FSpeechImportResult FSpeechImporter::Import(const FSpeechImportRequest& Request)
{
	FSpeechImportResult Result;

	if (!FPaths::FileExists(Request.AbsoluteAudioPath))
	{
		Result.Error = FString::Printf(TEXT("No audio at '%s'."), *Request.AbsoluteAudioPath);
		return Result;
	}

	const FString AssetName = SanitizeAssetName(Request.AssetName);
	const FString PackagePath = Request.DestinationPackagePath;

	FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));

	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = Request.AbsoluteAudioPath;
	Task->DestinationPath = PackagePath;
	Task->DestinationName = AssetName;
	Task->bAutomated = true;
	Task->bSave = false;

	// Replacing rather than creating a numbered sibling. A line owns one sound, addressed by its id,
	// and a regeneration that quietly produced SW_Line_1 next to SW_Line would leave the game playing
	// whichever one an old reference happened to point at.
	Task->bReplaceExisting = true;
	Task->bReplaceExistingSettings = false;

	USoundFactory* Factory = NewObject<USoundFactory>();
	Factory->bAutoCreateCue = false;
	Task->Factory = Factory;

	AssetTools.Get().ImportAssetTasks({ Task });

	USoundWave* Sound = nullptr;
	for (UObject* Object : Task->GetObjects())
	{
		Sound = Cast<USoundWave>(Object);
		if (Sound)
		{
			break;
		}
	}

	if (!Sound)
	{
		Result.Error = FString::Printf(
			TEXT("The import produced no sound asset from '%s'. The file may not be a container the ")
			TEXT("engine reads."),
			*Request.AbsoluteAudioPath);
		return Result;
	}

	Result.Sound = Sound;
	Result.ImportedDurationSeconds = Sound->Duration;
	Result.AudioHash = HashFile(Request.AbsoluteAudioPath);

	// ---------------------------------------------------------------------------------------------
	// Alignment, attached to the sound rather than only to the line
	// ---------------------------------------------------------------------------------------------

	if (!Request.Alignment.IsEmpty())
	{
		// UAssetUserData is the engine's own way of attaching data to an asset you do not own. A
		// facial pass and a shot-authoring tool both want word timings for a sound, and neither
		// should have to find the bank that produced it in order to get them.
		if (USpeechAlignmentUserData* Existing = Sound->GetAssetUserData<USpeechAlignmentUserData>())
		{
			Sound->RemoveUserDataOfClass(USpeechAlignmentUserData::StaticClass());
		}

		USpeechAlignmentUserData* UserData = NewObject<USpeechAlignmentUserData>(Sound);
		UserData->Alignment = Request.Alignment;
		UserData->LineId = Request.LineId;
		UserData->SpeakerId = Request.SpeakerId;
		UserData->SourceAssetPath = Request.SourceAssetPath;

		Sound->AddAssetUserData(UserData);
	}

	// ---------------------------------------------------------------------------------------------
	// The check that shares no code with the writer
	// ---------------------------------------------------------------------------------------------

	if (Request.bVerifyDuration && !Request.Alignment.IsEmpty())
	{
		Result.DurationDelta = Result.ImportedDurationSeconds - Request.Alignment.DurationSeconds;

		if (FMath::Abs(Result.DurationDelta) > Request.DurationToleranceSeconds)
		{
			// Deliberately a warning that travels with the result rather than a silent log line. The
			// import succeeded and the asset is real; what is in doubt is whether its timings mean
			// anything, and that is exactly the sort of thing that passes every other check.
			const FString Warning = FString::Printf(
				TEXT("Alignment says %.3fs and the imported sound is %.3fs (delta %.3fs). The audio ")
				TEXT("was resampled, arrived in an unexpected container, or the wrong alignment ")
				TEXT("variant was read. Timings on this sound should not be trusted."),
				Request.Alignment.DurationSeconds,
				Result.ImportedDurationSeconds,
				Result.DurationDelta);

			Result.Warnings.Add(Warning);
			UE_LOG(LogSpeechForge, Warning, TEXT("%s: %s"), *AssetName, *Warning);
		}
		else
		{
			UE_LOG(LogSpeechForge, Verbose,
				TEXT("%s: alignment and imported duration agree to %.4fs."), *AssetName, Result.DurationDelta);
		}
	}

	// ---------------------------------------------------------------------------------------------
	// Save
	// ---------------------------------------------------------------------------------------------

	Sound->MarkPackageDirty();

	UPackage* Package = Sound->GetOutermost();
	if (Package)
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;

		if (!UPackage::SavePackage(Package, nullptr, *Filename, SaveArgs))
		{
			Result.Warnings.Add(FString::Printf(TEXT("Imported but could not save '%s'."), *Filename));
		}
	}

	FAssetRegistryModule& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	AssetRegistry.Get().AssetCreated(Sound);

	Result.bSuccess = true;
	return Result;
}
