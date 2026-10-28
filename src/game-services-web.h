/**
* Copyright (c) 2020 CD Projekt Red. All Rights Reserved.
*/

#pragma once
#include "common/gameServices/include/gameServices.h"
#include "common/gameServices/include/gameServicesUser.h"
namespace services {
class GameServicesWebWorker;
	class GAME_SERVICES_API GameServicesWeb : public GameServices
	{
		RTTI_DECLARE_TYPE( GameServicesWeb );

	public:
		GameServicesWeb();
		~GameServicesWeb();

		virtual Bool Initialize( red::AppType appType ) override;
		virtual void Shutdown() override;
		virtual void Update() override;

		virtual red::Platform GetPlatform() const override;
		virtual red::StringView GetPlatformName() const override;

		virtual red::StringView GetLanguageCodeName() const override;

		virtual void AssignUser( Uint64 controllerID ) override;
		virtual Bool HasUserAssigned() const override;
		virtual IGameServicesUser* GetUser() const override { return m_user.Get(); }

	private:
		void OnUserAssigned();
		virtual void OnRequestSaves() override;
		virtual void OnSave( const save::MetadataPtr& metadata ) override;
		virtual void OnLoad( const save::MetadataPtr& metadata ) override;
		virtual void OnMetadataLoad( const GameSaveView& saveView ) override;
		virtual void OnRemoveSave( const GameSaveView& saveView ) override;
		virtual void OnRemoveSaves() override;
		virtual void OnAchievementUnlock( const THandle< game::data::Achievement_Record >& achievement, Float percentComplete ) override;
		virtual void OnRichPresenceSet( RichPresenceStatus status ) override;

		virtual void OnInGameConfigLoad() override;
		virtual void OnInGameConfigSave() override;

		virtual void OnUserFileSave( const red::String& fileName ) override;
		virtual void OnUserFileLoad( const red::String& fileName ) override;

		virtual void OnScreenshotCreate() override;

#if !defined(RED_CONFIGURATION_FINAL) && defined(RED_PLATFORM_WINPC)
		virtual void OnFakeSignout() override;
#endif

		virtual void OnOpenWebBrowser( const red::String& url ) override;

#if defined( RED_SAVE_SERVER_ENABLED )
		virtual void OnSaveDownload( const GameSaveView& saveView ) override;
		virtual void OnSaveUpload( red::UniqueBuffer&& save, const red::String& metadataName, const save::MetadataPtr& metadata, const img::ImagePtr& screenshot ) override;
#endif

		void OnSaveStarted();
		void OnSaveWaiting();
		void OnSaveProcess();
		void OnSaveFinished();

		void OnLoadStarted();
		void OnLoadWaiting();
		void OnLoadFinished();

		red::Mutex m_workerMutex;
		red::ConditionVariable m_wakeUpWorker;
		red::UniquePtr< GameServicesWebWorker > m_worker;
		red::UniquePtr<IGameServicesUser> m_user;
		red::String m_language;

		enum class State : Uint32;
		red::Atomic< State > m_state;
		red::Atomic< Bool > m_hasAssignedUser;
	};

}
