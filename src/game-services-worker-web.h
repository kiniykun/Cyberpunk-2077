/**
* Copyright (c) 2020 CD Projekt Red. All Rights Reserved.
*/

#pragma once

#include "common/gameServices/include/gameServicesWorker.h"
#include "common/redSystem/include/redThreadsThread.h"

namespace services
{
	struct GameSaveView;

	class GameServicesWebWorker final : public red::Thread
	{
		RED_USE_MEMORY_POOL( PoolGameServices );

	public:
		GameServicesWebWorker(
			red::Mutex& workerMutex,
			red::ConditionVariable& wakeUpWorker );

		void SavesGather( Uint32 maxAutoSavesCounter, Uint32 maxQuickSavesCounter, OnGameSavesGatheredCallback&& onGathered );

		void SavePrepare( save::Metadata& metadata, Bool willOverwrite, OnSavePreparedCallback&& onPrepared );
		void SaveProcess( const save::MetadataPtr& metadata, OnSaveFinishedCallback&& onFinished );

		void LoadPrepare( const save::MetadataPtr& metadata, ServiceError& errorCode, red::Atomic<Bool>& isFinished, OnLoadStartedCallback&& onPrepared );

		void MetadataLoad( const GameSaveView& saveView, OnLoadedMetadataCallback&& onLoaded );

		void SaveRemove( const GameSaveView& saveView, OnSaveRemovedCallback&& onRemoved );
		void SavesRemove( OnSavesRemovedCallback&& onRemoved );

		void ScreenshotCreate(
			Uint32& screenshotSize,
			red::UniqueBuffer& screenshotBuffer,
			OnScreenshotCreatedCallback&& onCreated );

#if defined( RED_SAVE_SERVER_ENABLED )
		void SaveDownload(
			GameSaveView& saveView,
			red::UniqueBuffer& saveBuffer,
			const save::MetadataPtr& metadata,
			OnGameSaveDownloadedCallback&& onDownloaded );

		void SaveUpload(
			red::UniqueBuffer&& save,
			const red::String& metadataName,
			const save::MetadataPtr& metadata,
			const img::ImagePtr& image,
			OnGameSaveUploadedCallback&& onUploaded );
#endif

		void Exit();

		virtual void ThreadFunc() override;

	private:
		class SavesGatherRequest final : public GameServicesWorkerGatherSavesRequest
		{
		public:
			SavesGatherRequest(
				Uint32 maxAutoSavesCount,
				Uint32 maxQuickSavesCount,
				OnGameSavesGatheredCallback&& onGathered );

			virtual void OnExecute() override;

			const Uint32 maxAutoSavesCount;
			const Uint32 maxQuickSavesCount;
		};

		class SavePrepareRequest final : public GameServicesWorkerPrepareSaveEnvironmentRequest
		{
		public:
			SavePrepareRequest(
				red::UniqueBuffer& buffer,
				save::Metadata& metadata,
				Bool willOverwrite,
				OnSavePreparedCallback&& onPrepared );

			virtual void OnExecute() override;

			red::UniqueBuffer& buffer;
		};

		class SaveProcessRequest final : public GameServicesWorkerProcessSavingRequest
		{
		public:
			SaveProcessRequest(
				red::UniqueBuffer& buffer,
				const save::MetadataPtr& metadata,
				OnSaveFinishedCallback&& onFinished );

			virtual void Execute() override;
			virtual void OnExecute() override;

			GameSaveView saveView;
			red::UniqueBuffer& buffer;
			red::UniquePtr< IFile > m_screenshotFile;
		};

		class LoadPrepareRequest final : public GameServicesWorkerPrepareLoadEnvironmentRequest
		{
		public:
			using GameServicesWorkerPrepareLoadEnvironmentRequest::GameServicesWorkerPrepareLoadEnvironmentRequest;

			virtual void OnExecute() override;
		};

		class MetadataLoadRequest final : public GameServicesWorkerLoadMetadataRequest
		{
		public:
			using GameServicesWorkerLoadMetadataRequest::GameServicesWorkerLoadMetadataRequest;

			virtual void OnExecute() override;
		};

		class SaveRemoveRequest final : public GameServicesWorkerRemoveSaveRequest
		{
		public:
			using GameServicesWorkerRemoveSaveRequest::GameServicesWorkerRemoveSaveRequest;

			virtual void OnExecute() override;
		};

		class SavesRemoveRequest final : public GameServicesWorkerRemoveSavesRequest
		{
		public:
			using GameServicesWorkerRemoveSavesRequest::GameServicesWorkerRemoveSavesRequest;

			virtual void OnExecute() override;
		};

		class ScreenshotCreateRequest final : public GameServicesWorkerRequest
		{
		public:
			ScreenshotCreateRequest(
				Uint32& screenshotSize,
				red::UniqueBuffer& screenshotBuffer,
				OnScreenshotCreatedCallback&& onCreated );

			virtual void Execute() override;
			virtual void OnExecute() override;

			Uint32& screenshotSize;
			red::UniqueBuffer& screenshotBuffer;
			OnScreenshotCreatedCallback onCreated;
			red::UniquePtr< IFile > screenshotFile;
		};

#if defined( RED_SAVE_SERVER_ENABLED )
		class SaveDownloadRequest final : public GameServicesWorkerDownloadSaveRequest
		{
		public:
			using GameServicesWorkerDownloadSaveRequest::GameServicesWorkerDownloadSaveRequest;

			virtual void OnExecute() override;
		};

		class SaveUploadRequest final : public GameServicesWorkerUploadSaveRequest
		{
		public:
			using GameServicesWorkerUploadSaveRequest::GameServicesWorkerUploadSaveRequest;
			virtual void OnExecute() override;
		};
#endif

		red::Mutex& m_workerMutex;
		red::ConditionVariable& m_wakeUpWorker;

		red::RWSpinLock m_requestsLock;
		red::DynArray< GameServicesWorkerRequestPtr > m_requests;
		GameServicesWorkerRequestPtr m_currentRequest;

		red::UniqueBuffer m_saveBuffer;

		red::Atomic<Bool> m_exit;
	};
}
