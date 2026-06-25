#pragma once

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <unordered_set>
#include <vector>

#include <pu/ui/ui_Application.hpp>

#include <app/AppConfig.hpp>
#include <catalog/QueueItem.hpp>
#include <catalog/InstalledTitle.hpp>
#include <catalog/RemoteCatalogState.hpp>
#include <i18n/I18n.hpp>
#include <platform/SystemStatus.hpp>
#include <platform/QueueDownloadWorker.hpp>
#include <platform/ForwarderInstaller.hpp>
#include <platform/AppUpdate.hpp>
#include <ui/ShellLayout.hpp>

namespace shield::ui {

class MainApplication : public pu::ui::Application {
    private:
        shield::app::AppConfig config_;
        shield::i18n::I18n translator_;
        std::vector<shield::catalog::InstalledTitle> installed_titles_;
        std::vector<shield::catalog::QueueItem> queue_items_;
        shield::catalog::RemoteCatalogState remote_catalog_state_;
        shield::platform::SystemOverview system_overview_;
        ShellLayout::Ref layout_;
        bool catalog_warning_shown_ = false;
        bool catalog_warning_pending_ = false;
        int catalog_warning_delay_frames_ = 0;
        int render_frame_count_ = 0;
        int load_stage_ = 0;
        bool data_loaded_ = false;
        bool remote_catalog_loaded_ = false;
        bool remote_catalog_loading_ = false;
        int remote_catalog_delay_frames_ = 0;
        std::future<shield::catalog::RemoteCatalogState> remote_catalog_future_;
        std::string remote_catalog_request_language_;
        std::atomic<bool> remote_catalog_cancel_requested_ = false;
        bool remote_catalog_update_block_logged_ = false;
        bool queue_download_loading_ = false;
        std::future<shield::platform::QueueDownloadResult> queue_download_future_;
        std::atomic<bool> queue_pause_requested_ = false;
        std::atomic<bool> queue_cancel_requested_ = false;
        std::string active_queue_title_id_;
        bool clear_active_queue_item_after_finish_ = false;
        bool auto_sleep_disabled_ = false;
        bool clear_all_queue_items_after_finish_ = false;
        bool applet_mode_ = false;
        bool applet_mode_notice_shown_ = false;
        bool applet_mode_notice_pending_ = false;
        int applet_mode_notice_delay_frames_ = 0;
        std::mutex task_progress_mutex_;
        TaskProgress task_progress_;
        bool task_progress_dirty_ = false;
        std::chrono::steady_clock::time_point last_queue_layout_refresh_at_;
        shield::app::AppSection observed_section_ = shield::app::AppSection::Installed;
        bool shutdown_requested_ = false;
        bool shutdown_close_issued_ = false;
        int remote_catalog_fail_count_ = 0;
        int shutdown_wait_frames_ = 0;
        int shutdown_exit_screen_frames_ = 0;
        bool shutdown_exit_screen_render_logged_ = false;
        bool shutdown_self_exit_requested_ = false;
        std::future<shield::platform::ForwarderInstallResult> forwarder_install_future_;
        bool forwarder_install_running_ = false;
        std::future<shield::platform::AppUpdateInfo> app_update_check_future_;
        std::future<shield::platform::AppUpdateResult> app_update_install_future_;
        shield::platform::AppUpdateInfo pending_app_update_;
        bool app_update_check_started_ = false;
        bool app_update_check_completed_ = false;
        bool app_update_prompt_shown_ = false;
        int app_update_check_fail_count_ = 0;
        bool app_update_install_requested_ = false;
        bool app_update_install_running_ = false;
        bool startup_catalog_gate_released_ = true;
        int initial_ui_ready_frames_ = 0;

        void QueueCatalogWarningIfNeeded();
        void AdvanceLoadingStage();
        void MaybeLoadRemoteCatalog();
        void MaybeFinishRemoteCatalogLoad();
        void MaybeStartQueueDownload();
        void MaybeFinishQueueDownload();
        void BeginShutdown();
        void MaybeCompleteShutdown();
        void RequestRenderLoopStop();
        void RunQueueControlAction(QueueControlAction action, const std::string &title_id);
        std::vector<shield::catalog::QueueItem> BuildDisplayedQueueItems();
        void ApplyLayoutData(bool refresh_remote_catalog = false);
        void SetTaskProgress(TaskProgress task_progress);
        void ClearTaskProgress();
        void MaybePushTaskProgressToLayout();
        void RefreshAutoSleepPolicy();        
        void QueueAppletModeNotice();
        void ShowAppletModeBlockedDialog();
        s32 ShowThemedDialog(const std::string &title, const std::string &content, const std::vector<std::string> &opts, bool use_last_opt_as_cancel = true, int initial_selected_option = -1);
        bool ConfirmQueueOptions(shield::catalog::QueueItem &item);
        bool ConfirmExitWhileQueueActive();
        void NormalizePendingQueueItemsForStartup();
        std::string ResolveBaseTitleId(const shield::catalog::QueueItem &item) const;
        bool IsRelatedContent(const shield::catalog::QueueItem &item) const;
        bool IsDlcContent(const shield::catalog::QueueItem &item) const;
        bool IsDlcInstalled(const std::string &title_id) const;
        bool IsBaseInstalledOrQueued(const std::string &base_title_id, const std::string &queued_title_id = {}) const;
        bool CanQueueWithoutBaseInstalled(const shield::catalog::QueueItem &item) const;
        bool ShouldQueueRelatedContentForItem(const shield::catalog::QueueItem &item) const;
        bool ShouldQueueSiblingDlcsForItem(const shield::catalog::QueueItem &item) const;
        void EnqueueItem(const shield::catalog::QueueItem &item, bool confirm_options = true, bool show_feedback = true);
        void EnqueueItems(const std::vector<shield::catalog::QueueItem> &items, bool confirm_options);
        void BeginForwarderInstall();
        void MaybeFinishForwarderInstall();
        void MaybeStartAppUpdateCheck();
        void MaybeFinishAppUpdateCheck();
        void BeginAppUpdateInstall();
        void MaybeFinishAppUpdateInstall();
        bool IsStartupCatalogGateRequired() const;
        bool IsInitialCatalogFlowComplete() const;
        bool IsInitialUiReadyForAppUpdateCheck();
        void UpdateStartupCatalogGate();
        void RefreshCatalogDerivedStateAfterInstall(const shield::catalog::QueueItem &item);


    public:
        using Application::Application;
        PU_SMART_CTOR(MainApplication)

        ~MainApplication() override;
        void OnLoad() override;
};

}
