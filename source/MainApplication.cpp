#include <ui/MainApplication.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <future>
#include <fstream>
#include <cctype>
#include <cerrno>
#include <unordered_map>
#include <sys/stat.h>

#include <switch.h>

#include <AppVersion.hpp>
#include <app/AppSection.hpp>
#include <app/QueueRepository.hpp>
#include <catalog/UpdateCandidate.hpp>
#include <platform/CustomIndexParser.hpp>
#include <platform/AppUpdate.hpp>
#include <platform/ExitLog.hpp>
#include <platform/DeviceIdentity.hpp>
#include <platform/InstalledTitleScanner.hpp>
#include <platform/RemoteCatalogClient.hpp>
#include <platform/QueueDownloadWorker.hpp>
#include <platform/RemoteImageCache.hpp>
#include <platform/SystemStatus.hpp>

#ifndef LITEFOIL_APP_VERSION
#define LITEFOIL_APP_VERSION "dev"
#endif

namespace shield::ui {
namespace {

bool IsRemoteUrl(const std::string &url) {
    return (url.rfind("https://", 0) == 0) || (url.rfind("http://", 0) == 0);
}

long long ElapsedMsSince(const std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
}

bool IsHexTitleId(const std::string &value) {
    if(value.size() != 16) {
        return false;
    }

    return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return std::isxdigit(character) != 0;
    });
}

std::string NormalizeTitleIdForCompare(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return value;
}

bool IsAppletModeRuntime() {
    return appletGetAppletType() != AppletType_Application;
}

std::uint32_t ExtractVersionFromUrl(const std::string &url) {
    const std::size_t marker_index = url.find("[v");
    if(marker_index == std::string::npos) {
        return 0;
    }

    const std::size_t end_index = url.find(']', marker_index + 2);
    if(end_index == std::string::npos) {
        return 0;
    }

    const std::string version = url.substr(marker_index + 2, end_index - (marker_index + 2));
    if(version.empty() || !std::all_of(version.begin(), version.end(), [](const unsigned char character) {
        return std::isdigit(character) != 0;
    })) {
        return 0;
    }

    return static_cast<std::uint32_t>(std::strtoul(version.c_str(), nullptr, 10));
}

bool UrlContainsTitleId(const std::string &url, const std::string &title_id) {
    return std::search(url.begin(), url.end(), title_id.begin(), title_id.end(),
        [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }
    ) != url.end();
}

int SourcePriority(const std::string &url) {
    if(url.rfind("gdrive:", 0) == 0) {
        return 0;
    }
    if(url.rfind("gdrivecrypt:", 0) == 0) {
        return 1;
    }
    if((url.rfind("https://", 0) == 0) || (url.rfind("http://", 0) == 0)) {
        return url.find("drive.google.com") == std::string::npos ? 2 : 3;
    }
    return 4;
}

std::string FindBestFileUrlForTitle(const std::vector<shield::catalog::RemoteFileEntry> &files, const std::string &title_id) {
    const shield::catalog::RemoteFileEntry *best_file = nullptr;
    std::uint32_t best_version = 0;
    int best_priority = 100;

    for(const auto &file : files) {
        if(!UrlContainsTitleId(file.url, title_id)) {
            continue;
        }

        const std::uint32_t file_version = ExtractVersionFromUrl(file.url);
        const int priority = SourcePriority(file.url);
        if((best_file == nullptr) || (file_version > best_version) || ((file_version == best_version) && (priority < best_priority))) {
            best_file = std::addressof(file);
            best_version = file_version;
            best_priority = priority;
        }
    }

    return best_file == nullptr ? "" : best_file->url;
}

std::string BuildQueueItemIdentity(const shield::catalog::QueueItem &item) {
    constexpr char kSeparator = '\x1F';
    return item.title_id + kSeparator
        + item.base_title_id + kSeparator
        + item.name + kSeparator
        + item.source_url + kSeparator
        + item.package_format + kSeparator
        + std::to_string(item.size);
}

struct DialogTheme {
    pu::ui::Color dialog_color;
    pu::ui::Color title_color;
    pu::ui::Color content_color;
    pu::ui::Color option_color;
    pu::ui::Color over_color;
};

DialogTheme ResolveDialogTheme(const shield::app::AppConfig &config) {
    if(config.theme == "light") {
        return {
            { 0xF4, 0xF5, 0xF7, 0xFF },
            { 0x18, 0x1A, 0x1F, 0xFF },
            { 0x41, 0x46, 0x52, 0xFF },
            { 0xF2, 0xF4, 0xF8, 0xFF },
            { 0xD6, 0xE6, 0xFF, 0xFF }
        };
    }
    if(config.theme == "dark") {
        return {
            { 0x1D, 0x22, 0x2B, 0xFF },
            { 0xF3, 0xF6, 0xFA, 0xFF },
            { 0xC4, 0xCD, 0xD8, 0xFF },
            { 0x2C, 0x34, 0x42, 0xFF },
            { 0x49, 0x6E, 0x9B, 0xFF }
        };
    }

    return {
        { 0xEC, 0xEE, 0xF2, 0xFF },
        { 0x1D, 0x22, 0x29, 0xFF },
        { 0x4E, 0x55, 0x62, 0xFF },
        { 0xF2, 0xF4, 0xF8, 0xFF },
        { 0xD9, 0xE8, 0xFF, 0xFF }
    };
}

std::string BuildQueueOptionsDialogBody(const shield::i18n::I18n &translator, const shield::catalog::QueueItem &item) {
    std::ostringstream body;
    body << item.name;
    if(!item.subtitle.empty()) {
        body << "\n" << item.subtitle;
    }

    if(item.size > 0) {
        body << "\n" << translator.Get("info.size") << ": " << shield::platform::SystemStatus::FormatStorageAmount(item.size);
    }

    return body.str();
}

std::string BuildToggleLabel(const std::string &label, bool value, const shield::i18n::I18n &translator) {
    return label + ": " + (value ? translator.Get("settings.common.enabled") : translator.Get("settings.common.disabled"));
}

std::string BuildInstallationModelLabel(const shield::i18n::I18n &translator, const shield::catalog::QueueItem &item) {
    return translator.Get("info.installationModel") + ": "
        + translator.Get(item.installation_model == "stream" ? "settings.installationModel.stream" : "settings.installationModel.direct");
}

shield::catalog::RemoteCatalogState LoadRemoteCatalogStateWorker(const shield::app::AppConfig &config,
    const std::string &language_tag,
    const std::vector<shield::catalog::InstalledTitle> &installed_titles,
    const std::string &device_uid,
    std::function<bool()> cancel_callback) {
    shield::catalog::RemoteCatalogState remote_catalog_state;
    remote_catalog_state.device_uid = device_uid;
    if(!config.catalog_uid_override.empty()) {
        remote_catalog_state.device_uid = config.catalog_uid_override;
    }

    remote_catalog_state.source_url = config.catalog_url;
    remote_catalog_state.source_title = config.catalog_title;
    remote_catalog_state.configured = !config.catalog_url.empty();

    if(!remote_catalog_state.configured) {
        return remote_catalog_state;
    }

    std::optional<shield::catalog::RemoteCatalog> parser_result;
    if(!IsRemoteUrl(config.catalog_url)) {
        const auto parse_start = std::chrono::steady_clock::now();
        parser_result = shield::platform::CustomIndexParser::ParseFile(config.catalog_url);
        shield::platform::RuntimeLog("[catalog] parse file finished elapsed_ms=%lld success=%d",
            ElapsedMsSince(parse_start), static_cast<int>(parser_result.has_value()));
    }

    if(!parser_result.has_value() && IsRemoteUrl(config.catalog_url)) {
        const auto fetch_start = std::chrono::steady_clock::now();
        const auto fetch_result = shield::platform::RemoteCatalogClient::Fetch(
            config.catalog_url,
            language_tag,
            remote_catalog_state.device_uid,
            config.catalog_username,
            config.catalog_password,
            LITEFOIL_APP_VERSION,
            "0",
            std::move(cancel_callback));
        shield::platform::RuntimeLog("[catalog] fetch finished elapsed_ms=%lld success=%d bytes=%zu status=%ld",
            ElapsedMsSince(fetch_start),
            static_cast<int>(fetch_result.success),
            fetch_result.body.size(),
            fetch_result.response_code);

        if(fetch_result.success) {
            const auto parse_start = std::chrono::steady_clock::now();
            parser_result = shield::platform::CustomIndexParser::ParseString(fetch_result.body);
            shield::platform::RuntimeLog("[catalog] parse response finished elapsed_ms=%lld success=%d",
                ElapsedMsSince(parse_start), static_cast<int>(parser_result.has_value()));
            if(!parser_result.has_value()) {
                remote_catalog_state.status_message = "__parse_failed__";
            }
        }
        else {
            remote_catalog_state.status_message = fetch_result.error_message;
        }
    }

    if(parser_result.has_value()) {
        remote_catalog_state.loaded = true;
        remote_catalog_state.catalog = std::move(parser_result.value());
        const auto planner_start = std::chrono::steady_clock::now();
        remote_catalog_state.update_candidates = shield::catalog::UpdatePlanner::Build(installed_titles, remote_catalog_state.catalog);
        shield::platform::RuntimeLog("[catalog] update planner finished elapsed_ms=%lld titles=%zu files=%zu updates=%zu recommended=%zu",
            ElapsedMsSince(planner_start),
            remote_catalog_state.catalog.titles_by_id.size(),
            remote_catalog_state.catalog.files.size(),
            remote_catalog_state.update_candidates.size(),
            remote_catalog_state.catalog.recommended_titles.size());
        remote_catalog_state.warning_message = remote_catalog_state.catalog.success_message;
    }

    return remote_catalog_state;
}

}

MainApplication::~MainApplication() {
    if(this->auto_sleep_disabled_) {
        appletSetAutoSleepDisabled(false);
        this->auto_sleep_disabled_ = false;
    }
}

void MainApplication::QueueAppletModeNotice() {
    if(!this->applet_mode_) {
        return;
    }

    this->applet_mode_notice_pending_ = true;
    this->applet_mode_notice_delay_frames_ = 0;
}

void MainApplication::ShowAppletModeBlockedDialog() {
    this->ShowThemedDialog(
        this->translator_.Get("dialog.appletModeTitle"),
        this->translator_.Get("dialog.appletModeBlockedBody"),
        { this->translator_.Get("common.ok") },
        true);
}

bool MainApplication::IsStartupCatalogGateRequired() const {
    return this->remote_catalog_state_.configured && !this->applet_mode_;
}

bool MainApplication::IsInitialCatalogFlowComplete() const {
    if(!this->data_loaded_) {
        return false;
    }
    if(!this->remote_catalog_state_.configured || this->applet_mode_) {
        return true;
    }

    return this->remote_catalog_loaded_;
}

bool MainApplication::IsInitialUiReadyForAppUpdateCheck() {
    if(!this->IsInitialCatalogFlowComplete() || !this->startup_catalog_gate_released_) {
        this->initial_ui_ready_frames_ = 0;
        return false;
    }

    if(this->initial_ui_ready_frames_ < 2) {
        this->initial_ui_ready_frames_++;
        return false;
    }

    return true;
}

void MainApplication::UpdateStartupCatalogGate() {
    if(this->layout_ == nullptr) {
        return;
    }
    if(this->shutdown_requested_) {
        return;
    }

    const bool gate_required = this->IsStartupCatalogGateRequired();
    if(!gate_required) {
        this->startup_catalog_gate_released_ = true;
        this->layout_->ApplyStartupGate(false, "", "", 0.0f);
        return;
    }

    if(this->remote_catalog_loaded_) {
        this->startup_catalog_gate_released_ = true;
    }

    if(this->startup_catalog_gate_released_) {
        this->layout_->ApplyStartupGate(false, "", "", 1.0f);
        return;
    }

    float ratio = 0.08f;
    std::string detail = this->remote_catalog_state_.status_message;
    if(detail.empty()) {
        detail = this->translator_.Get("catalog.statusLoading");
    }

    if(!this->data_loaded_) {
        ratio = std::clamp(0.10f + (static_cast<float>(this->load_stage_) * 0.10f), 0.10f, 0.32f);
    }
    else if(this->remote_catalog_loading_) {
        ratio = 0.68f;
    }
    else if(this->remote_catalog_fail_count_ > 0) {
        ratio = 0.42f;
    }

    this->layout_->ApplyStartupGate(true, this->translator_.Get("catalog.statusLoading"), detail, ratio);
}

void MainApplication::QueueCatalogWarningIfNeeded() {
    if(this->remote_catalog_state_.warning_message.empty()) {
        return;
    }

    this->catalog_warning_pending_ = true;
    this->catalog_warning_delay_frames_ = 0;
}

void MainApplication::AdvanceLoadingStage() {
    if(this->data_loaded_) {
        return;
    }

    switch(this->load_stage_) {
        case 0:
            this->remote_catalog_state_.status_message = this->translator_.Get("catalog.statusLoadingLibrary");
            this->ApplyLayoutData();
            this->installed_titles_ = shield::platform::InstalledTitleScanner::Scan();
            this->load_stage_++;
            return;
        case 1:
            this->remote_catalog_state_.status_message = this->translator_.Get("catalog.statusLoadingSystem");
            this->ApplyLayoutData();
            this->system_overview_ = shield::platform::SystemStatus::ReadOverview();
            this->load_stage_++;
            return;
        case 2:
            if(this->applet_mode_ && this->remote_catalog_state_.configured) {
                this->remote_catalog_state_.status_message = this->translator_.Get("catalog.statusAppletBlocked");
            } else {
                this->remote_catalog_state_.status_message = this->remote_catalog_state_.configured
                    ? this->translator_.Get("catalog.statusDeferred")
                    : this->translator_.Get("catalog.statusNotConfigured");
            }
            this->ApplyLayoutData();
            this->data_loaded_ = true;
            this->load_stage_++;
            return;
        default:
            this->data_loaded_ = true;
            return;
    }
}

void MainApplication::MaybeLoadRemoteCatalog() {
    if(this->shutdown_requested_
        || this->app_update_install_requested_
        || this->app_update_install_running_
        || this->remote_catalog_loaded_
        || !this->remote_catalog_state_.configured
        || this->applet_mode_) {
        if((this->app_update_install_requested_ || this->app_update_install_running_)
            && !this->remote_catalog_update_block_logged_) {
            this->remote_catalog_update_block_logged_ = true;
            shield::platform::RuntimeLog("[catalog] MaybeLoadRemoteCatalog: blocked by app update requested=%d running=%d",
                static_cast<int>(this->app_update_install_requested_),
                static_cast<int>(this->app_update_install_running_));
        }
        return;
    }

    if(!this->remote_catalog_loading_) {
        this->remote_catalog_update_block_logged_ = false;
        shield::platform::RuntimeLog("[catalog] MaybeLoadRemoteCatalog: starting");
        this->remote_catalog_loading_ = true;
        this->remote_catalog_state_.status_message = this->translator_.Get("catalog.statusLoading");
        if(this->remote_catalog_state_.device_uid.empty()) {
            this->remote_catalog_state_.device_uid = shield::platform::DeviceIdentity::ComputeUidFromMmcCid();
            if(!this->config_.catalog_uid_override.empty()) {
                this->remote_catalog_state_.device_uid = this->config_.catalog_uid_override;
            }
        }

          const auto config = this->config_;
          const auto installed_titles = this->installed_titles_;
          const std::string language_tag = this->translator_.GetLanguageTag();
          const std::string device_uid = this->remote_catalog_state_.device_uid;
          this->remote_catalog_cancel_requested_ = false;
          this->remote_catalog_request_language_ = language_tag;
          this->remote_catalog_future_ = std::async(std::launch::async, [this, config, installed_titles, language_tag, device_uid]() {
              return LoadRemoteCatalogStateWorker(config, language_tag, installed_titles, device_uid, [this]() {
                  return this->remote_catalog_cancel_requested_.load();
              });
          });
          this->ApplyLayoutData();
          return;
      }
}

void MainApplication::MaybeFinishRemoteCatalogLoad() {
    if(!this->remote_catalog_loading_ || !this->remote_catalog_future_.valid()) {
        return;
    }

    if(this->remote_catalog_future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }

    auto loaded_state = this->remote_catalog_future_.get();
    this->remote_catalog_loading_ = false;

    shield::platform::RuntimeLog("[catalog] MaybeFinishRemoteCatalogLoad: loaded=%d status=%s app_update_requested=%d",
        static_cast<int>(loaded_state.loaded),
        loaded_state.status_message.c_str(),
        static_cast<int>(this->app_update_install_requested_));

    if(this->app_update_install_requested_ || this->app_update_install_running_) {
        shield::platform::RuntimeLog("[catalog] MaybeFinishRemoteCatalogLoad: ignored because app update is active");
        return;
    }

    if((loaded_state.source_url != this->config_.catalog_url) || (this->remote_catalog_request_language_ != this->translator_.GetLanguageTag())) {
        return;
    }

    this->remote_catalog_state_ = std::move(loaded_state);
    if(this->remote_catalog_state_.loaded) {
        if(this->remote_catalog_state_.status_message == "__cached__") {
            this->remote_catalog_state_.status_message = this->translator_.Get("catalog.statusLoaded");
        }
        else {
            this->remote_catalog_state_.status_message = this->translator_.Get("catalog.statusLoaded");
        }

        if(!this->remote_catalog_state_.source_title.empty()) {
            this->remote_catalog_state_.status_message += " " + this->remote_catalog_state_.source_title;
        }
    }
    else if(this->remote_catalog_state_.status_message == "__parse_failed__") {
        this->remote_catalog_state_.status_message = this->translator_.Get("catalog.statusParseFailed");
    }
    else if(this->remote_catalog_state_.status_message.empty()) {
        this->remote_catalog_state_.status_message = this->translator_.Get("catalog.statusUnavailable");
    }

    const auto apply_start = std::chrono::steady_clock::now();
    this->ApplyLayoutData(true);
    shield::platform::RuntimeLog("[catalog] layout apply finished elapsed_ms=%lld",
        ElapsedMsSince(apply_start));
    this->QueueCatalogWarningIfNeeded();
    this->remote_catalog_loaded_ = this->remote_catalog_state_.loaded;
    if(!this->remote_catalog_loaded_) {
        this->remote_catalog_fail_count_++;
        if(this->remote_catalog_fail_count_ >= 3) {
            this->remote_catalog_state_.status_message = this->translator_.Get("catalog.statusRetryLimit");
            this->remote_catalog_loaded_ = true;
            this->ApplyLayoutData(true);
        } else {
            this->remote_catalog_delay_frames_ = 0;
        }
    } else {
        for(auto &q : this->queue_items_) {
            if(q.state == shield::catalog::QueueItemState::Completed
                || q.state == shield::catalog::QueueItemState::Canceled) continue;
            const std::string resolved_url = FindBestFileUrlForTitle(this->remote_catalog_state_.catalog.files, q.title_id);
            if(!resolved_url.empty() && resolved_url != q.source_url) {
                q.source_url = resolved_url;
                if(q.state == shield::catalog::QueueItemState::Failed) {
                    q.retry_count = 0;
                    q.last_error.clear();
                }
            }
        }
    }
}

void MainApplication::MaybeStartQueueDownload() {
    if(this->shutdown_requested_
        || this->applet_mode_
        || this->app_update_install_requested_
        || this->app_update_install_running_) {
        return;
    }

    if(this->queue_download_loading_) {
        return;
    }

    auto next_item = std::find_if(this->queue_items_.begin(), this->queue_items_.end(), [](const shield::catalog::QueueItem &item) {
        return item.auto_start
            && ((item.state == shield::catalog::QueueItemState::Queued)
                || ((item.state == shield::catalog::QueueItemState::Failed) && (item.retry_count < item.retry_limit)));
    });
    if(next_item == this->queue_items_.end()) {
        return;
    }

    // Preserve partial progress when the user resumes a paused item. Everything
    // else restarts from zero so stale counters do not leak between retries.
    const bool resuming_partial = (next_item->state == shield::catalog::QueueItemState::Paused) && (next_item->bytes_done > 0);
    next_item->state = shield::catalog::QueueItemState::Downloading;
    if(!resuming_partial) {
        next_item->bytes_done = 0;
    }
    next_item->bytes_total = next_item->size;
    shield::app::QueueRepository::Save(this->queue_items_);
    this->ApplyLayoutData();

    const auto item = *next_item;
    this->active_queue_title_id_ = item.title_id;
    this->queue_pause_requested_ = false;
    this->queue_cancel_requested_ = false;
    this->clear_active_queue_item_after_finish_ = false;
    const std::string progress_title = this->translator_.Get("task.queue.title") + ": " + item.name;
    this->SetTaskProgress(TaskProgress{
        true,
        progress_title,
        this->translator_.Get("task.queue.preparing"),
        0.0f
    });
    this->queue_download_loading_ = true;
    const shield::platform::InstallBridgeConfig bridge_config{
        this->config_.allow_unsigned_sources,
        shield::platform::LoadRcloneCryptConfig(this->config_)
    };
    this->queue_download_future_ = std::async(std::launch::async, [this, item, progress_title, bridge_config]() {
        return shield::platform::QueueDownloadWorker::Run(item, "sdmc:/switch/LiteFoil/downloads", bridge_config,
            [this, progress_title, item](const shield::platform::DownloadProgress &progress) {
                const float ratio = (progress.bytes_total > 0)
                    ? static_cast<float>(progress.bytes_done) / static_cast<float>(progress.bytes_total)
                    : 0.0f;
                std::ostringstream detail;
                if(progress.bytes_total > 0) {
                    detail << shield::platform::SystemStatus::FormatStorageAmount(progress.bytes_done)
                        << " / "
                        << shield::platform::SystemStatus::FormatStorageAmount(progress.bytes_total);
                }
                else {
                    detail << shield::platform::SystemStatus::FormatStorageAmount(progress.bytes_done);
                }
                if(progress.bytes_per_second > 0.0) {
                    detail << "  |  "
                           << shield::platform::SystemStatus::FormatStorageAmount(static_cast<std::uint64_t>(progress.bytes_per_second))
                           << "/s";
                }

                this->SetTaskProgress(TaskProgress{
                    true,
                    progress_title,
                    detail.str(),
                    ratio,
                    item.title_id,
                    progress.bytes_done,
                    progress.bytes_total,
                    progress.bytes_per_second,
                    false
                });
            },
            [this, progress_title, item](const shield::install::InstallProgress &progress) {
                const float ratio = (progress.bytes_total > 0)
                    ? static_cast<float>(progress.bytes_done) / static_cast<float>(progress.bytes_total)
                    : 0.0f;

                std::ostringstream detail;
                detail << this->translator_.Get(progress.decompressing
                    ? "task.queue.decompressing"
                    : "task.queue.installing");
                if(progress.nca_count > 0) {
                    detail << "  |  " << progress.nca_index << "/" << progress.nca_count;
                }
                if(progress.bytes_total > 0) {
                    detail << "  |  "
                           << shield::platform::SystemStatus::FormatStorageAmount(progress.bytes_done)
                           << " / "
                           << shield::platform::SystemStatus::FormatStorageAmount(progress.bytes_total);
                }
                if(!progress.current_nca.empty()) {
                    detail << "  |  " << progress.current_nca;
                }

                this->SetTaskProgress(TaskProgress{
                    true,
                    progress_title,
                    detail.str(),
                    ratio,
                    item.title_id,
                    progress.bytes_done,
                    progress.bytes_total,
                    progress.bytes_per_second,
                    true
                });
            },
            [this]() {
                if(this->queue_cancel_requested_) {
                    return shield::platform::DownloadStopReason::Canceled;
                }
                if(this->queue_pause_requested_) {
                    return shield::platform::DownloadStopReason::Paused;
                }
                return shield::platform::DownloadStopReason::None;
            });
    });
}

void MainApplication::MaybeFinishQueueDownload() {
    if(!this->queue_download_loading_ || !this->queue_download_future_.valid()) {
        return;
    }

    if(this->queue_download_future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }

    const auto result = this->queue_download_future_.get();
    this->queue_download_loading_ = false;
    const std::string finished_title_id = this->active_queue_title_id_;
    this->active_queue_title_id_.clear();
    this->queue_pause_requested_ = false;
    this->queue_cancel_requested_ = false;
    this->ClearTaskProgress();

    const auto item_it = std::find_if(this->queue_items_.begin(), this->queue_items_.end(), [&result](const shield::catalog::QueueItem &item) {
        return item.title_id == result.item.title_id;
    });
    if(item_it != this->queue_items_.end()) {
        *item_it = result.item;
    }

    if(this->clear_all_queue_items_after_finish_) {
        this->queue_items_.erase(std::remove_if(this->queue_items_.begin(), this->queue_items_.end(), [](const shield::catalog::QueueItem &item) {
            return item.state == shield::catalog::QueueItemState::Completed ||
                item.state == shield::catalog::QueueItemState::Canceled ||
                item.state == shield::catalog::QueueItemState::Failed;
        }), this->queue_items_.end());
        this->clear_all_queue_items_after_finish_ = false;
    }
    else if(this->clear_active_queue_item_after_finish_ && !finished_title_id.empty()) {
        this->queue_items_.erase(std::remove_if(this->queue_items_.begin(), this->queue_items_.end(), [&finished_title_id](const shield::catalog::QueueItem &item) {
            return item.title_id == finished_title_id;
        }), this->queue_items_.end());
        this->clear_active_queue_item_after_finish_ = false;
    }

    shield::app::QueueRepository::Save(this->queue_items_);
    if(result.success && (result.item.state == shield::catalog::QueueItemState::Completed)) {
        this->RefreshCatalogDerivedStateAfterInstall(result.item);
        return;
    }

    this->ApplyLayoutData();
}

void MainApplication::RefreshCatalogDerivedStateAfterInstall(const shield::catalog::QueueItem &item) {
    shield::platform::RuntimeLog("[catalog] RefreshCatalogDerivedStateAfterInstall: title_id=%s",
        item.title_id.c_str());

    this->installed_titles_ = shield::platform::InstalledTitleScanner::Scan();
    this->system_overview_ = shield::platform::SystemStatus::ReadOverview();
    if(this->remote_catalog_state_.loaded) {
        this->remote_catalog_state_.update_candidates = shield::catalog::UpdatePlanner::Build(
            this->installed_titles_,
            this->remote_catalog_state_.catalog);
    }

    this->ApplyLayoutData(true);
}

void MainApplication::BeginForwarderInstall() {
    if(this->forwarder_install_running_ || this->queue_download_loading_) {
        return;
    }

    if(shield::platform::ForwarderInstaller::IsInstalled()) {
        if(this->layout_ != nullptr) {
            this->layout_->SetForwarderActivated(true);
        }
        return;
    }

    this->forwarder_install_running_ = true;

    TaskProgress progress;
    progress.active = true;
    progress.title = this->translator_.Get("forwarder.progress.title");
    progress.detail = this->translator_.Get("forwarder.progress.detail");
    progress.ratio = 0.0f;
    this->SetTaskProgress(progress);

    this->forwarder_install_future_ = std::async(std::launch::async, [this]() {
        return shield::platform::ForwarderInstaller::Install(
            [this](float ratio) {
                TaskProgress p;
                p.active = true;
                p.title = this->translator_.Get("forwarder.progress.title");
                p.detail = this->translator_.Get("forwarder.progress.detail");
                p.ratio = ratio;
                this->SetTaskProgress(p);
            });
    });
}

void MainApplication::MaybeFinishForwarderInstall() {
    if(!this->forwarder_install_running_ || !this->forwarder_install_future_.valid()) {
        return;
    }

    if(this->forwarder_install_future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }

    const auto result = this->forwarder_install_future_.get();
    this->forwarder_install_running_ = false;
    this->ClearTaskProgress();

    if(result.success) {
        if(this->layout_ != nullptr) {
            this->layout_->SetForwarderActivated(true);
        }
        this->ShowThemedDialog(
            this->translator_.Get("forwarder.dialog.title"),
            this->translator_.Get("forwarder.dialog.successBody"),
            { this->translator_.Get("common.ok") });
    }
    else {
        this->ShowThemedDialog(
            this->translator_.Get("forwarder.dialog.title"),
            this->translator_.Get("forwarder.dialog.errorBody") + "\n" + result.error_message,
            { this->translator_.Get("common.ok") });
    }
}

void MainApplication::MaybeStartAppUpdateCheck() {
    if(this->shutdown_requested_
        || this->applet_mode_
        || this->app_update_check_started_
        || this->app_update_check_completed_
        || this->app_update_prompt_shown_
        || this->app_update_check_fail_count_ >= 3
        || this->app_update_install_requested_
        || this->app_update_install_running_
        || this->config_.update_url.empty()) {
        return;
    }

    this->app_update_check_started_ = true;
    shield::platform::RuntimeLog("[app-update-ui] MaybeStartAppUpdateCheck: starting attempt=%d",
        this->app_update_check_fail_count_ + 1);
    const std::string update_url = this->config_.update_url;
    this->app_update_check_future_ = std::async(std::launch::async, [update_url]() {
        return shield::platform::AppUpdate::Check(update_url, LITEFOIL_APP_VERSION);
    });
}

void MainApplication::MaybeFinishAppUpdateCheck() {
    if(!this->app_update_check_started_ || !this->app_update_check_future_.valid()) {
        return;
    }
    if(this->app_update_check_future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }

    const auto info = this->app_update_check_future_.get();
    this->app_update_check_started_ = false;
    shield::platform::RuntimeLog("[app-update-ui] MaybeFinishAppUpdateCheck: available=%d version=%s message=%s",
        static_cast<int>(info.available), info.version.c_str(), info.message.c_str());
    if(!info.available && !info.message.empty()) {
        this->app_update_check_fail_count_++;
        shield::platform::RuntimeLog("[app-update-ui] MaybeFinishAppUpdateCheck: failed attempt=%d",
            this->app_update_check_fail_count_);
        if(this->app_update_check_fail_count_ >= 3) {
            this->app_update_check_completed_ = true;
            shield::platform::RuntimeLog("[app-update-ui] MaybeFinishAppUpdateCheck: giving up, catalog released");
        }
        return;
    }

    if(!info.available || this->app_update_prompt_shown_) {
        this->app_update_check_completed_ = true;
        shield::platform::RuntimeLog("[app-update-ui] MaybeFinishAppUpdateCheck: no update, catalog released");
        return;
    }

    this->pending_app_update_ = info;
    this->app_update_prompt_shown_ = true;
    std::string body = this->translator_.Get("appUpdate.available") + info.version
        + "\n" + this->translator_.Get("appUpdate.current") + LITEFOIL_APP_VERSION;

    const auto result = this->ShowThemedDialog(
        this->translator_.Get("appUpdate.title"),
        body,
        {
            this->translator_.Get("appUpdate.install"),
            this->translator_.Get("appUpdate.later")
        },
        true);
    if(result == 0) {
        this->BeginAppUpdateInstall();
    }
    else {
        this->app_update_check_completed_ = true;
        shield::platform::RuntimeLog("[app-update-ui] MaybeFinishAppUpdateCheck: user skipped update, catalog released");
    }
}

void MainApplication::BeginAppUpdateInstall() {
    shield::platform::RuntimeLog("[app-update-ui] BeginAppUpdateInstall: enter running=%d queue=%d forwarder=%d",
        static_cast<int>(this->app_update_install_running_),
        static_cast<int>(this->queue_download_loading_),
        static_cast<int>(this->forwarder_install_running_));
    if(this->app_update_install_running_) {
        return;
    }
    if(this->queue_download_loading_ || this->forwarder_install_running_) {
        this->ShowThemedDialog(
            this->translator_.Get("appUpdate.title"),
            this->translator_.Get("appUpdate.busy"),
            { this->translator_.Get("common.ok") },
            true);
        return;
    }
    if(!this->pending_app_update_.available || this->pending_app_update_.url.empty()) {
        shield::platform::RuntimeLog("[app-update-ui] BeginAppUpdateInstall: no pending update");
        return;
    }

    this->app_update_install_requested_ = true;
    if(this->remote_catalog_loading_) {
        shield::platform::RuntimeLog("[app-update-ui] BeginAppUpdateInstall: canceling remote catalog before update");
        this->remote_catalog_cancel_requested_ = true;
        if(this->remote_catalog_future_.valid()) {
            for(int attempt = 0; attempt < 100; ++attempt) {
                if(this->remote_catalog_future_.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready) {
                    auto canceled_state = this->remote_catalog_future_.get();
                    this->remote_catalog_loading_ = false;
                    shield::platform::RuntimeLog("[app-update-ui] BeginAppUpdateInstall: remote catalog stopped loaded=%d status=%s",
                        static_cast<int>(canceled_state.loaded),
                        canceled_state.status_message.c_str());
                    break;
                }
            }
        }
        if(this->remote_catalog_loading_) {
            shield::platform::RuntimeLog("[app-update-ui] BeginAppUpdateInstall: remote catalog did not stop, aborting update start");
            this->app_update_install_requested_ = false;
            this->ShowThemedDialog(
                this->translator_.Get("appUpdate.title"),
                this->translator_.Get("appUpdate.busy"),
                { this->translator_.Get("common.ok") },
                true);
            return;
        }
    }

    this->app_update_install_running_ = true;
    shield::platform::RuntimeLog("[app-update-ui] BeginAppUpdateInstall: starting worker version=%s",
        this->pending_app_update_.version.c_str());
    const std::string progress_title = this->translator_.Get("appUpdate.progress.download");
    const std::string extract_detail = this->translator_.Get("appUpdate.progress.extract");
    const std::string install_detail = this->translator_.Get("appUpdate.progress.install");
    const std::string download_detail = this->pending_app_update_.version;
    TaskProgress initial_progress;
    initial_progress.active = true;
    initial_progress.title = progress_title;
    initial_progress.detail = download_detail;
    initial_progress.ratio = 0.0f;
    this->SetTaskProgress(initial_progress);

    const auto info = this->pending_app_update_;
    this->app_update_install_future_ = std::async(std::launch::async, [this, info, progress_title, download_detail, extract_detail, install_detail]() {
        shield::platform::RuntimeLog("[app-update-ui] worker: enter");
        return shield::platform::AppUpdate::DownloadAndInstall(info,
            [this, progress_title, download_detail, extract_detail, install_detail](const shield::platform::AppUpdateProgress &progress) {
                TaskProgress task_progress;
                task_progress.active = true;
                task_progress.title = progress_title;
                if(progress.stage == "extract") {
                    task_progress.detail = extract_detail;
                }
                else if(progress.stage == "install") {
                    task_progress.detail = install_detail;
                }
                else {
                    task_progress.detail = download_detail;
                }
                task_progress.ratio = static_cast<float>(std::max(0.0, std::min(1.0, progress.ratio)));
                task_progress.bytes_done = progress.bytes_done;
                task_progress.bytes_total = progress.bytes_total;
                this->SetTaskProgress(task_progress);
            });
    });
}

void MainApplication::MaybeFinishAppUpdateInstall() {
    if(!this->app_update_install_running_ || !this->app_update_install_future_.valid()) {
        return;
    }
    if(this->app_update_install_future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }

    const auto result = this->app_update_install_future_.get();
    shield::platform::RuntimeLog("[app-update-ui] MaybeFinishAppUpdateInstall: worker result success=%d message=%s installed=%s",
        static_cast<int>(result.success), result.message.c_str(), result.installed_path.c_str());
    this->app_update_install_running_ = false;
    if(result.success) {
        shield::platform::RuntimeLog("[app-update-ui] MaybeFinishAppUpdateInstall: update installed, showing completion dialog before shutdown");
        this->ClearTaskProgress();
        this->ShowThemedDialog(
            this->translator_.Get("appUpdate.title"),
            this->translator_.Get("appUpdate.success"),
            { this->translator_.Get("common.ok") },
            true);
        shield::platform::RuntimeLog("[app-update-ui] MaybeFinishAppUpdateInstall: completion dialog closed, stopping render loop");
        if(this->auto_sleep_disabled_) {
            appletSetAutoSleepDisabled(false);
            this->auto_sleep_disabled_ = false;
        }
        this->shutdown_requested_ = true;
        this->shutdown_close_issued_ = true;
        this->RequestRenderLoopStop();
        shield::platform::RuntimeLog("[app-update-ui] MaybeFinishAppUpdateInstall: render loop stop requested");
        return;
    }

    shield::platform::RuntimeLog("[app-update-ui] MaybeFinishAppUpdateInstall: showing failure dialog");
    this->ClearTaskProgress();
    this->app_update_install_requested_ = false;
    this->ShowThemedDialog(
        this->translator_.Get("appUpdate.title"),
        this->translator_.Get("appUpdate.failed") + result.message,
        { this->translator_.Get("common.ok") },
        true);
}

void MainApplication::BeginShutdown() {
    if(this->shutdown_requested_) {
        shield::platform::ExitLog("BeginShutdown: already requested, ignoring");
        return;
    }

    shield::platform::ExitLogReset();
    shield::platform::ExitLog("BeginShutdown: entry (catalog_loading=%d queue_loading=%d)",
        static_cast<int>(this->remote_catalog_loading_),
        static_cast<int>(this->queue_download_loading_));

    this->shutdown_requested_ = true;
    this->shutdown_close_issued_ = false;
    this->shutdown_exit_screen_frames_ = 0;
    this->shutdown_exit_screen_render_logged_ = false;
    this->shutdown_self_exit_requested_ = false;
    this->remote_catalog_cancel_requested_ = this->remote_catalog_loading_;
    this->queue_cancel_requested_ = this->queue_download_loading_;
    this->queue_pause_requested_ = false;
    if(this->layout_ != nullptr) {
        shield::platform::ExitLog("BeginShutdown: showing exit screen");
        this->layout_->SetShuttingDown(true);
    }

    shield::platform::ExitLog("BeginShutdown: cancel signals set, calling RemoteImageCache::Shutdown()");
    shield::platform::RemoteImageCache::Shutdown();
    shield::platform::ExitLog("BeginShutdown: RemoteImageCache::Shutdown() returned");

    bool queue_changed = false;
    for(auto &item : this->queue_items_) {
        if(item.auto_start) {
            item.auto_start = false;
            queue_changed = true;
        }
        if(item.bytes_per_second != 0.0) {
            item.bytes_per_second = 0.0;
            queue_changed = true;
        }
    }

    if(queue_changed) {
        shield::app::QueueRepository::Save(this->queue_items_);
        this->ApplyLayoutData();
    }

    this->MaybeCompleteShutdown();
}

void MainApplication::MaybeCompleteShutdown() {
    if(!this->shutdown_requested_ || this->shutdown_close_issued_) {
        return;
    }

    if(this->shutdown_exit_screen_frames_ < 6) {
        return;
    }

    if(this->remote_catalog_loading_ || this->queue_download_loading_ || this->app_update_install_running_) {
        if(++this->shutdown_wait_frames_ < 180) {
            if((this->shutdown_wait_frames_ % 30) == 1) {
                shield::platform::ExitLog("MaybeCompleteShutdown: waiting (frame=%d catalog=%d queue=%d app_update=%d)",
                    this->shutdown_wait_frames_,
                    static_cast<int>(this->remote_catalog_loading_),
                    static_cast<int>(this->queue_download_loading_),
                    static_cast<int>(this->app_update_install_running_));
            }
            return;
        }
        shield::platform::ExitLog("MaybeCompleteShutdown: wait timeout at frame=%d, proceeding", this->shutdown_wait_frames_);
    }

    shield::platform::ExitLog("MaybeCompleteShutdown: proceeding (catalog_loading=%d queue_loading=%d app_update=%d)",
        static_cast<int>(this->remote_catalog_loading_),
        static_cast<int>(this->queue_download_loading_),
        static_cast<int>(this->app_update_install_running_));

    if(this->auto_sleep_disabled_) {
        appletSetAutoSleepDisabled(false);
        this->auto_sleep_disabled_ = false;
    }

    this->shutdown_close_issued_ = true;

    shield::platform::ExitLog("MaybeCompleteShutdown: keeping exit screen visible, skipping FadeOut()");

    if((appletGetAppletType() == AppletType_Application) && !this->shutdown_self_exit_requested_) {
        this->shutdown_self_exit_requested_ = true;
        shield::platform::ExitLog("MaybeCompleteShutdown: before appletRequestExitToSelf");
        const Result self_exit_rc = appletRequestExitToSelf();
        shield::platform::ExitLog("MaybeCompleteShutdown: appletRequestExitToSelf rc=0x%x", self_exit_rc);
    }

    // Drain background futures so worker threads are done with sockets/services
    // before main returns and libnx runs userAppExit().
    if(this->remote_catalog_future_.valid()) {
        shield::platform::ExitLog("MaybeCompleteShutdown: waiting on remote_catalog_future_");
        this->remote_catalog_future_.wait();
        shield::platform::ExitLog("MaybeCompleteShutdown: remote_catalog_future_ resolved");
    }
    if(this->queue_download_future_.valid()) {
        shield::platform::ExitLog("MaybeCompleteShutdown: waiting on queue_download_future_");
        this->queue_download_future_.wait();
        shield::platform::ExitLog("MaybeCompleteShutdown: queue_download_future_ resolved");
    }
    if(this->app_update_check_future_.valid()) {
        shield::platform::ExitLog("MaybeCompleteShutdown: waiting on app_update_check_future_");
        this->app_update_check_future_.wait();
        shield::platform::ExitLog("MaybeCompleteShutdown: app_update_check_future_ resolved");
    }
    if(this->app_update_install_future_.valid()) {
        shield::platform::ExitLog("MaybeCompleteShutdown: waiting on app_update_install_future_");
        this->app_update_install_future_.wait();
        shield::platform::ExitLog("MaybeCompleteShutdown: app_update_install_future_ resolved");
    }

    // Graceful shutdown: stop the render loop and let ShowWithFadeIn() return
    // to main. Calling Close() here finalizes the renderer from inside
    // CallForRender(), then Plutonium still tries to finalize the current
    // frame, which can crash on exit.
    //
    // The previous socketExit hang (libcurl's cached keep-alive TCP
    // connections blocking bsdExit) is prevented by ShutdownCurl() being
    // called in userAppExit before socketExit().
    shield::platform::ExitLog("MaybeCompleteShutdown: stopping render loop");
    shield::platform::RuntimeLog("[shutdown] MaybeCompleteShutdown: stopping render loop");
    this->RequestRenderLoopStop();
    shield::platform::ExitLog("MaybeCompleteShutdown: render loop stop requested");
    shield::platform::RuntimeLog("[shutdown] MaybeCompleteShutdown: render loop stop requested");
}

void MainApplication::RequestRenderLoopStop() {
    this->is_shown = false;
}

void MainApplication::RunQueueControlAction(const QueueControlAction action, const std::string &title_id) {
    const std::string selected_identity = title_id;
    auto target_it = std::find_if(this->queue_items_.begin(), this->queue_items_.end(), [&selected_identity](const shield::catalog::QueueItem &item) {
        return BuildQueueItemIdentity(item) == selected_identity;
    });

    if(action == QueueControlAction::PauseResumeAll) {
        const auto is_running_or_pending = [](const shield::catalog::QueueItem &item) {
            return (item.state == shield::catalog::QueueItemState::Queued)
                || (item.state == shield::catalog::QueueItemState::Downloading)
                || (item.state == shield::catalog::QueueItemState::Installing);
        };

        const bool should_pause = std::any_of(this->queue_items_.begin(), this->queue_items_.end(), is_running_or_pending);
        if(this->applet_mode_ && !should_pause) {
            this->ShowAppletModeBlockedDialog();
            return;
        }

        if(should_pause) {
            if(this->queue_download_loading_) {
                this->queue_pause_requested_ = true;
            }

            for(auto &item : this->queue_items_) {
                if(item.state == shield::catalog::QueueItemState::Queued) {
                    item.state = shield::catalog::QueueItemState::Paused;
                }
                if((item.state == shield::catalog::QueueItemState::Paused)
                    || (item.state == shield::catalog::QueueItemState::Downloading)
                    || (item.state == shield::catalog::QueueItemState::Installing)) {
                    item.auto_start = false;
                    item.bytes_per_second = 0.0;
                }
            }
        }
        else {
            for(auto &item : this->queue_items_) {
                if((item.state == shield::catalog::QueueItemState::Paused)
                    || (item.state == shield::catalog::QueueItemState::Failed)) {
                    item.state = shield::catalog::QueueItemState::Queued;
                    item.auto_start = true;
                    item.last_error.clear();
                    item.bytes_per_second = 0.0;
                }
            }
        }

        shield::app::QueueRepository::Save(this->queue_items_);
        this->ApplyLayoutData();
        return;
    }

    if(action == QueueControlAction::CancelAll) {
        if(this->queue_download_loading_) {
            // Active downloads finish their current curl callback and then stop in a
            // controlled way, so we only mark inactive items immediately here.
            this->queue_cancel_requested_ = true;
        }

        for(auto &item : this->queue_items_) {
            if(item.state == shield::catalog::QueueItemState::Completed) {
                continue;
            }
            if((item.state == shield::catalog::QueueItemState::Downloading)
                || (item.state == shield::catalog::QueueItemState::Installing)) {
                item.auto_start = false;
                item.bytes_per_second = 0.0;
                continue;
            }
            item.state = shield::catalog::QueueItemState::Canceled;
            item.auto_start = false;
            item.bytes_done = 0;
            item.bytes_per_second = 0.0;
            item.last_error.clear();
            item.local_path.clear();
        }

        shield::app::QueueRepository::Save(this->queue_items_);
        this->ApplyLayoutData();
        return;
    }

    if(action == QueueControlAction::ClearAll) {
        this->clear_all_queue_items_after_finish_ = false;
        this->queue_items_.erase(std::remove_if(this->queue_items_.begin(), this->queue_items_.end(), [](const shield::catalog::QueueItem &item) {
            return item.state == shield::catalog::QueueItemState::Completed ||
                item.state == shield::catalog::QueueItemState::Canceled ||
                item.state == shield::catalog::QueueItemState::Failed;
        }), this->queue_items_.end());
        shield::app::QueueRepository::Save(this->queue_items_);
        this->ApplyLayoutData();
        return;
    }

    if(target_it == this->queue_items_.end()) {
        return;
    }

    auto &item = *target_it;
    const bool is_active_item = this->queue_download_loading_ && (item.title_id == this->active_queue_title_id_);

    if((action == QueueControlAction::MoveUp) || (action == QueueControlAction::MoveDown)) {
        const auto index = static_cast<std::size_t>(std::distance(this->queue_items_.begin(), target_it));
        if((action == QueueControlAction::MoveUp) && (index > 0)) {
            std::swap(this->queue_items_[index], this->queue_items_[index - 1]);
        }
        else if((action == QueueControlAction::MoveDown) && ((index + 1) < this->queue_items_.size())) {
            std::swap(this->queue_items_[index], this->queue_items_[index + 1]);
        }
        else {
            return;
        }
        shield::app::QueueRepository::Save(this->queue_items_);
        this->ApplyLayoutData();
        return;
    }

    if(this->applet_mode_ && (action == QueueControlAction::PauseResume) && !is_active_item) {
        this->ShowAppletModeBlockedDialog();
        return;
    }

    switch(action) {
        case QueueControlAction::PauseResume:
            if(is_active_item && ((item.state == shield::catalog::QueueItemState::Downloading)
                || (item.state == shield::catalog::QueueItemState::Installing))) {
                this->queue_pause_requested_ = true;
                item.auto_start = false;
                item.bytes_per_second = 0.0;
            }
            else if(item.state == shield::catalog::QueueItemState::Queued) {
                item.state = shield::catalog::QueueItemState::Paused;
                item.auto_start = false;
                item.bytes_per_second = 0.0;
            }
            else if((item.state == shield::catalog::QueueItemState::Paused)
                || (item.state == shield::catalog::QueueItemState::Failed)) {
                item.state = shield::catalog::QueueItemState::Queued;
                item.auto_start = true;
                item.last_error.clear();
                item.bytes_per_second = 0.0;
            }
            break;
        case QueueControlAction::Cancel:
            if(is_active_item && ((item.state == shield::catalog::QueueItemState::Downloading)
                || (item.state == shield::catalog::QueueItemState::Installing))) {
                this->queue_cancel_requested_ = true;
                item.auto_start = false;
                item.bytes_per_second = 0.0;
            }
            else {
                item.state = shield::catalog::QueueItemState::Canceled;
                item.auto_start = false;
                item.bytes_done = 0;
                item.bytes_per_second = 0.0;
                item.last_error.clear();
                item.local_path.clear();
            }
            break;
        case QueueControlAction::ClearItem:
            if(is_active_item && ((item.state == shield::catalog::QueueItemState::Downloading)
                || (item.state == shield::catalog::QueueItemState::Installing))) {
                this->queue_cancel_requested_ = true;
                this->clear_active_queue_item_after_finish_ = true;
                item.auto_start = false;
                item.bytes_per_second = 0.0;
            }
            else {
                this->queue_items_.erase(target_it);
            }
            break;
        case QueueControlAction::CancelAll:
        case QueueControlAction::ClearAll:
        case QueueControlAction::PauseResumeAll:
        case QueueControlAction::MoveUp:
        case QueueControlAction::MoveDown:
        default:
            break;
    }

    shield::app::QueueRepository::Save(this->queue_items_);
    this->ApplyLayoutData();
}

std::vector<shield::catalog::QueueItem> MainApplication::BuildDisplayedQueueItems() {
    return this->queue_items_;
}

void MainApplication::ApplyLayoutData(const bool refresh_remote_catalog) {
    if(this->layout_ == nullptr) {
        return;
    }
    this->layout_->ApplyLoadedData(this->installed_titles_, this->BuildDisplayedQueueItems(), this->remote_catalog_state_, this->system_overview_, refresh_remote_catalog);
}

void MainApplication::SetTaskProgress(TaskProgress task_progress) {
    std::scoped_lock lock(this->task_progress_mutex_);
    this->task_progress_ = std::move(task_progress);
    this->task_progress_dirty_ = true;
}

void MainApplication::ClearTaskProgress() {
    this->SetTaskProgress(TaskProgress{});
}

void MainApplication::MaybePushTaskProgressToLayout() {
    if(this->layout_ == nullptr) {
        return;
    }

    TaskProgress task_progress;
    {
        std::scoped_lock lock(this->task_progress_mutex_);
        if(!this->task_progress_dirty_) {
            return;
        }

        task_progress = this->task_progress_;
        this->task_progress_dirty_ = false;
    }

    if(!task_progress.active_title_id.empty()) {
        for(auto &q : this->queue_items_) {
            if(q.title_id == task_progress.active_title_id) {
                const bool stop_pending = (q.title_id == this->active_queue_title_id_)
                    && (this->queue_pause_requested_ || this->queue_cancel_requested_);
                if(!stop_pending) {
                    q.state = task_progress.is_installing
                        ? shield::catalog::QueueItemState::Installing
                        : shield::catalog::QueueItemState::Downloading;
                }
                q.bytes_done        = task_progress.bytes_done;
                q.bytes_total       = task_progress.bytes_total;
                q.bytes_per_second  = task_progress.bytes_per_second;
                break;
            }
        }
    }

    this->layout_->ApplyTaskProgress(task_progress);

    if(this->layout_->GetSelectedSection() == shield::app::AppSection::Queue) {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - this->last_queue_layout_refresh_at_).count();
        if(elapsed_ms >= 1500) {
            this->last_queue_layout_refresh_at_ = now;
            this->layout_->ApplyQueueData(this->BuildDisplayedQueueItems());
        }
    }
}

void MainApplication::RefreshAutoSleepPolicy() {
    const bool should_disable_auto_sleep = this->queue_download_loading_ || this->app_update_install_running_;
    if(this->auto_sleep_disabled_ == should_disable_auto_sleep) {
        return;
    }

    if(R_SUCCEEDED(appletSetAutoSleepDisabled(should_disable_auto_sleep))) {
        this->auto_sleep_disabled_ = should_disable_auto_sleep;
    }
}

s32 MainApplication::ShowThemedDialog(const std::string &title, const std::string &content, const std::vector<std::string> &opts, const bool use_last_opt_as_cancel, const int initial_selected_option) {
    const auto theme = ResolveDialogTheme(this->config_);
    return this->CreateShowDialog(title, content, opts, use_last_opt_as_cancel, {}, [theme, initial_selected_option](pu::ui::Dialog::Ref &dialog) {
        dialog->SetDialogColor(theme.dialog_color);
        dialog->SetTitleColor(theme.title_color);
        dialog->SetContentColor(theme.content_color);
        dialog->SetOptionColor(theme.title_color);
        dialog->SetOverColor(theme.over_color);
        dialog->SetDialogBorderRadius(38);
        dialog->SetOptionBorderRadius(18);
        dialog->SetFadeAlphaIncrementSteps(20);
        if(initial_selected_option >= 0) {
            dialog->SetSelectedOptionIndex(static_cast<u32>(initial_selected_option));
        }
    });
}

bool MainApplication::ConfirmQueueOptions(shield::catalog::QueueItem &item) {
    const std::string title    = this->translator_.Get("dialog.installOptionsTitle");
    const std::string body     = BuildQueueOptionsDialogBody(this->translator_, item);
    const std::string confirm  = this->translator_.Get("dialog.installOptionsConfirm");
    const std::string cancel   = this->translator_.Get("dialog.installOptionsCancel");

    // Interactive loop – each selection toggles the corresponding option and
    // re-displays the dialog so the user can review all changes before confirming.
    for(;;) {
        const std::string loc_label = this->translator_.Get("info.location") + ": "
            + (item.target_location == "NAND" ? "NAND" : "SD");
        const std::string reinstall_label = BuildToggleLabel(
            this->translator_.Get("info.reinstallNcas"), item.reinstall_ncas, this->translator_);
        const std::string install_model_label = BuildInstallationModelLabel(this->translator_, item);
        const std::string keep_label = BuildToggleLabel(
            this->translator_.Get("info.keepFile"), item.keep_download, this->translator_);
        const std::string dlcs_label = BuildToggleLabel(
            this->translator_.Get("info.includeAllDlcs"), item.include_all_dlcs, this->translator_);
        const std::string update_label = BuildToggleLabel(
            this->translator_.Get("info.includeLatestUpdate"), item.include_latest_update, this->translator_);

        std::vector<std::string> options = {
            loc_label, reinstall_label, install_model_label
        };
        const int keep_option = (item.installation_model == "stream") ? -1 : static_cast<int>(options.size());
        if(keep_option >= 0) {
            options.push_back(keep_label);
        }
        const int dlcs_option = static_cast<int>(options.size());
        options.push_back(dlcs_label);
        const int update_option = static_cast<int>(options.size());
        options.push_back(update_label);
        const int confirm_option = static_cast<int>(options.size());
        options.push_back(confirm);
        options.push_back(cancel);

        const auto result = this->ShowThemedDialog(title, body, options, true, confirm_option);

        switch(result) {
            case 0: item.target_location = (item.target_location == "NAND") ? "SD" : "NAND"; break;
            case 1: item.reinstall_ncas = !item.reinstall_ncas; break;
            case 2: break;
            default:
                if(result == keep_option) {
                    item.keep_download = !item.keep_download;
                    item.delete_after_download = !item.keep_download;
                }
                else if(result == dlcs_option) {
                    item.include_all_dlcs = !item.include_all_dlcs;
                }
                else if(result == update_option) {
                    item.include_latest_update = !item.include_latest_update;
                }
                else if(result == confirm_option) {
                    if(item.installation_model == "stream") {
                        item.keep_download = false;
                        item.delete_after_download = true;
                    }
                    return true;
                }
                else {
                    return false;
                }
                break;
        }
    }
}

bool MainApplication::ConfirmExitWhileQueueActive() {
    if(this->app_update_install_running_) {
        this->ShowThemedDialog(
            this->translator_.Get("appUpdate.title"),
            this->translator_.Get("appUpdate.busy"),
            { this->translator_.Get("common.ok") },
            true);
        return false;
    }
    if(!this->queue_download_loading_) {
        return true;
    }

    const auto result = this->ShowThemedDialog(
        this->translator_.Get("dialog.exitWhileQueueActiveTitle"),
        this->translator_.Get("dialog.exitWhileQueueActiveBody"),
        {
            this->translator_.Get("footer.exit"),
            this->translator_.Get("dialog.installOptionsCancel")
        },
        true);

    return result == 0;
}

void MainApplication::NormalizePendingQueueItemsForStartup() {
    bool changed = false;

    for(auto &item : this->queue_items_) {
        switch(item.state) {
            case shield::catalog::QueueItemState::Downloading:
            case shield::catalog::QueueItemState::Installing:
                item.state = shield::catalog::QueueItemState::Paused;
                item.auto_start = false;
                item.bytes_per_second = 0.0;
                changed = true;
                break;
            case shield::catalog::QueueItemState::Queued:
            case shield::catalog::QueueItemState::Paused:
            case shield::catalog::QueueItemState::Failed:
                if(item.auto_start || (item.bytes_per_second != 0.0)) {
                    item.auto_start = false;
                    item.bytes_per_second = 0.0;
                    changed = true;
                }
                break;
            case shield::catalog::QueueItemState::Completed:
            case shield::catalog::QueueItemState::Canceled:
            default:
                if(item.bytes_per_second != 0.0) {
                    item.bytes_per_second = 0.0;
                    changed = true;
                }
                break;
        }
    }

    if(changed) {
        shield::app::QueueRepository::Save(this->queue_items_);
    }
}

std::string MainApplication::ResolveBaseTitleId(const shield::catalog::QueueItem &item) const {
    if(IsHexTitleId(item.base_title_id)) {
        return item.base_title_id;
    }

    if(IsHexTitleId(item.title_id)) {
        // Zero the lower 12 bits (last 3 hex digits) to get the base game ID.
        return item.title_id.substr(0, 13) + "000";
    }

    return {};
}

bool MainApplication::IsRelatedContent(const shield::catalog::QueueItem &item) const {
    const auto found = this->remote_catalog_state_.catalog.titles_by_id.find(item.title_id);
    if(found != this->remote_catalog_state_.catalog.titles_by_id.end()) {
        return (found->second.content_type == shield::catalog::RemoteContentType::Update)
            || (found->second.content_type == shield::catalog::RemoteContentType::Dlc);
    }

    if(!IsHexTitleId(item.title_id)) {
        return false;
    }

    // Nintendo title IDs: lower 12 bits = content type (0x000 = base, 0x800 = update, else DLC).
    // Comparing only the last 3 hex digits avoids misclassifying IDs like "A000" as related content.
    return std::stoul(item.title_id.substr(13, 3), nullptr, 16) != 0;
}

bool MainApplication::IsDlcContent(const shield::catalog::QueueItem &item) const {
    const auto found = this->remote_catalog_state_.catalog.titles_by_id.find(item.title_id);
    if(found != this->remote_catalog_state_.catalog.titles_by_id.end()) {
        return found->second.content_type == shield::catalog::RemoteContentType::Dlc;
    }

    if(!IsHexTitleId(item.title_id)) {
        return false;
    }

    const auto content_bits = std::stoul(item.title_id.substr(13, 3), nullptr, 16);
    return content_bits != 0 && content_bits != 0x800;
}

bool MainApplication::IsDlcInstalled(const std::string &title_id) const {
    if(title_id.empty()) {
        return false;
    }

    const std::string normalized_title_id = NormalizeTitleIdForCompare(title_id);
    for(const auto &installed_title : this->installed_titles_) {
        for(const auto &add_on_title_id : installed_title.add_on_title_ids) {
            if(NormalizeTitleIdForCompare(add_on_title_id) == normalized_title_id) {
                return true;
            }
        }
    }

    return false;
}

bool MainApplication::IsBaseInstalledOrQueued(const std::string &base_title_id, const std::string &queued_title_id) const {
    if(base_title_id.empty()) {
        return true;
    }

    const auto installed = std::find_if(this->installed_titles_.begin(), this->installed_titles_.end(), [&base_title_id](const shield::catalog::InstalledTitle &title) {
        return title.title_id_hex == base_title_id;
    });
    if(installed != this->installed_titles_.end()) {
        return true;
    }

    const auto queued = std::find_if(this->queue_items_.begin(), this->queue_items_.end(), [&base_title_id, &queued_title_id](const shield::catalog::QueueItem &item) {
        if(item.title_id == queued_title_id) {
            return false;
        }
        if(item.title_id != base_title_id) {
            return false;
        }
        return item.state != shield::catalog::QueueItemState::Canceled;
    });

    return queued != this->queue_items_.end();
}

bool MainApplication::CanQueueWithoutBaseInstalled(const shield::catalog::QueueItem &item) const {
    if(!this->IsRelatedContent(item)) {
        return true;
    }

    return this->IsBaseInstalledOrQueued(this->ResolveBaseTitleId(item), item.title_id);
}

bool MainApplication::ShouldQueueRelatedContentForItem(const shield::catalog::QueueItem &item) const {
    if(this->IsRelatedContent(item)) {
        return false;
    }

    const auto base_title_id = this->ResolveBaseTitleId(item);
    return !base_title_id.empty() && (item.title_id == base_title_id);
}

bool MainApplication::ShouldQueueSiblingDlcsForItem(const shield::catalog::QueueItem &item) const {
    if(!item.include_all_dlcs || !this->IsDlcContent(item)) {
        return false;
    }

    const auto base_title_id = this->ResolveBaseTitleId(item);
    return !base_title_id.empty() && base_title_id.size() == 16;
}

void MainApplication::EnqueueItem(const shield::catalog::QueueItem &item, const bool confirm_options, const bool show_feedback) {
    if(this->applet_mode_) {
        if(show_feedback) {
            this->ShowAppletModeBlockedDialog();
        }
        return;
    }

    if(item.title_id.empty()) {
        return;
    }

    const auto duplicate = std::find_if(this->queue_items_.begin(), this->queue_items_.end(), [&item](const shield::catalog::QueueItem &existing) {
        return existing.title_id == item.title_id;
    });
    if(duplicate != this->queue_items_.end()) {
        if(show_feedback) {
            this->ShowThemedDialog(
                this->translator_.Get("dialog.queueTitle"),
                this->translator_.Get("dialog.queueAlreadyQueued"),
                { this->translator_.Get("common.ok") },
                true);
        }
        return;
    }

    auto queued_item = item;
    if(confirm_options && !this->ConfirmQueueOptions(queued_item)) {
        return;
    }

    if(!this->CanQueueWithoutBaseInstalled(queued_item)) {
            this->ShowThemedDialog(
                this->translator_.Get("dialog.queueTitle"),
                this->translator_.Get("dialog.queueRequiresBaseInstalled"),
                { this->translator_.Get("common.ok") },
                true);
            return;
        }

    queued_item.state = shield::catalog::QueueItemState::Queued;
    queued_item.bytes_total = queued_item.size;
    queued_item.bytes_done = 0;
    this->queue_items_.push_back(queued_item);

    std::vector<std::string> enqueued_names;
    enqueued_names.push_back(queued_item.name);

    // Nintendo title ID convention:
    //   Base game:  XXXXXXXXXXXX0000  (last 4 hex = 0000)
    //   Update:     XXXXXXXXXXXX0800  (last 4 hex = 0800)
    //   DLC:        XXXXXXXXXXXX0001 .. XXXXXXXXXXXX0FFF
    // base_title_id should point to the base game.  If it's empty, infer it.
    const std::string base_id = this->ResolveBaseTitleId(queued_item);

    const auto &catalog = this->remote_catalog_state_.catalog;
    const bool queue_related_content_for_base = this->ShouldQueueRelatedContentForItem(queued_item);
    const bool queue_sibling_dlcs = this->ShouldQueueSiblingDlcsForItem(queued_item);

    auto try_enqueue_related = [&](const std::string &related_id) {
        if(related_id == queued_item.title_id) return;
        for(const auto &q : this->queue_items_) {
            if(q.title_id == related_id) return;
        }
        const auto it = catalog.titles_by_id.find(related_id);
        if(it == catalog.titles_by_id.end()) return;

        const auto &related = it->second;
        if((related.content_type == shield::catalog::RemoteContentType::Dlc) && this->IsDlcInstalled(related.id)) {
            return;
        }

        shield::catalog::QueueItem related_item;
        related_item.title_id = related.id;
        related_item.base_title_id = related.base_title_id.empty() ? base_id : related.base_title_id;
        related_item.name = related.name.empty() ? related.id : related.name;
        related_item.subtitle = related.publisher;
        related_item.package_format = related.package_format;
        related_item.size = related.size;
        related_item.target_location = queued_item.target_location;
        related_item.delete_after_download = queued_item.delete_after_download;
        related_item.installation_model = queued_item.installation_model;
        related_item.keep_download = queued_item.keep_download;
        related_item.verify_integrity = queued_item.verify_integrity;
        related_item.reinstall_ncas = queued_item.reinstall_ncas;
        related_item.include_all_dlcs = false;
        related_item.include_latest_update = false;
        related_item.auto_start = true;
        related_item.retry_limit = 2;
        related_item.state = shield::catalog::QueueItemState::Queued;
        related_item.bytes_total = related.size;

        related_item.source_url = FindBestFileUrlForTitle(catalog.files, related.id);

        if(!related_item.source_url.empty()) {
            this->queue_items_.push_back(related_item);
            enqueued_names.push_back(related_item.name);
        }
    };

    auto is_dlc_for_base = [&base_id](const std::string &tid, const shield::catalog::RemoteTitleMetadata &meta) {
        if(meta.content_type != shield::catalog::RemoteContentType::Dlc) {
            return false;
        }
        if(!meta.base_title_id.empty()
            && NormalizeTitleIdForCompare(meta.base_title_id) == NormalizeTitleIdForCompare(base_id)) {
            return true;
        }
        if(!IsHexTitleId(tid) || !IsHexTitleId(base_id)) {
            return false;
        }

        const auto tid_content_bits = std::stoul(tid.substr(13, 3), nullptr, 16);
        return tid.substr(0, 13) == base_id.substr(0, 13)
            && tid_content_bits != 0
            && tid_content_bits != 0x800;
    };

    if(queue_related_content_for_base && queued_item.include_latest_update && !base_id.empty() && base_id.size() == 16) {
        // Update title ID: set 0x800 in the lower 12 bits (last 3 hex digits → "800").
        const std::string update_id = base_id.substr(0, 13) + "800";
        try_enqueue_related(update_id);
    }

    if((queue_related_content_for_base || queue_sibling_dlcs) && queued_item.include_all_dlcs && !base_id.empty() && base_id.size() == 16) {
        // Scan the entire catalog for DLC entries related to this base title.
        for(const auto &[tid, meta] : catalog.titles_by_id) {
            if(is_dlc_for_base(tid, meta)) {
                try_enqueue_related(tid);
            }
        }
    }

    shield::app::QueueRepository::Save(this->queue_items_);
    this->ApplyLayoutData();

    std::string summary;
    for(const auto &n : enqueued_names) {
        if(!summary.empty()) summary += "\n";
        summary += n;
    }
    this->ShowThemedDialog(
        this->translator_.Get("dialog.queueTitle"),
        this->translator_.Get("dialog.queueAdded") + "\n" + summary,
        { this->translator_.Get("common.ok") },
        true);
}

void MainApplication::EnqueueItems(const std::vector<shield::catalog::QueueItem> &items, const bool confirm_options) {
    if(this->applet_mode_) {
        this->ShowAppletModeBlockedDialog();
        return;
    }

    if(items.empty()) {
        return;
    }

    bool changed = false;
    bool options_confirmed = !confirm_options;

    for(const auto &item : items) {
        if(item.title_id.empty()) {
            continue;
        }

        const auto duplicate = std::find_if(this->queue_items_.begin(), this->queue_items_.end(), [&item](const shield::catalog::QueueItem &existing) {
            return existing.title_id == item.title_id;
        });
        if(duplicate != this->queue_items_.end()) {
            continue;
        }

        auto queued_item = item;
        if(confirm_options && !options_confirmed) {
            if(!this->ConfirmQueueOptions(queued_item)) {
                return;
            }
            options_confirmed = true;
        }

        if(!this->CanQueueWithoutBaseInstalled(queued_item)) {
            continue;
        }

        queued_item.state = shield::catalog::QueueItemState::Queued;
        queued_item.bytes_total = queued_item.size;
        queued_item.bytes_done = 0;
        this->queue_items_.push_back(std::move(queued_item));
        changed = true;
    }

    if(!changed) {
        return;
    }

    shield::app::QueueRepository::Save(this->queue_items_);
    this->ApplyLayoutData();
}

void MainApplication::OnLoad() {
    shield::platform::AppUpdate::CleanupCache();
    this->config_ = shield::app::AppConfigRepository::LoadOrCreate();
    this->applet_mode_ = IsAppletModeRuntime();
    this->queue_items_ = shield::app::QueueRepository::Load();
    this->NormalizePendingQueueItemsForStartup();

    const std::string preferred_language = shield::platform::SystemStatus::DetectPreferredLanguageTag(this->config_.language);
    this->translator_.Load(preferred_language);
    this->remote_catalog_state_.source_url = this->config_.catalog_url;
    this->remote_catalog_state_.source_title = this->config_.catalog_title;
    this->remote_catalog_state_.configured = !this->config_.catalog_url.empty();
    if(this->applet_mode_ && this->remote_catalog_state_.configured) {
        this->remote_catalog_state_.status_message = this->translator_.Get("catalog.statusAppletBlocked");
        this->remote_catalog_loaded_ = true;
    } else {
        this->remote_catalog_state_.status_message = this->remote_catalog_state_.configured
            ? this->translator_.Get("catalog.statusDeferred")
            : this->translator_.Get("catalog.statusNotConfigured");
        this->remote_catalog_loaded_ = !this->remote_catalog_state_.configured;
    }
    this->startup_catalog_gate_released_ = !this->IsStartupCatalogGateRequired() || this->remote_catalog_loaded_;

    this->layout_ = ShellLayout::New(this->config_, this->translator_, this->installed_titles_, this->queue_items_, this->remote_catalog_state_, this->system_overview_, this->applet_mode_);
    this->layout_->SetDialogCallback([this](const std::string &title, const std::string &body) {
        if(title.empty() || body.empty()) {
            return;
        }

        this->ShowThemedDialog(title, body, { this->translator_.Get("common.ok") }, true);
    });
    this->layout_->SetConfigChangedCallback([this](const shield::app::AppConfig &updated_config) {
        const bool language_changed = this->config_.language != updated_config.language;
        const bool update_url_changed = this->config_.update_url != updated_config.update_url;
        const bool remote_catalog_changed =
            (this->config_.catalog_url != updated_config.catalog_url)
            || (this->config_.catalog_title != updated_config.catalog_title)
            || (this->config_.catalog_username != updated_config.catalog_username)
            || (this->config_.catalog_password != updated_config.catalog_password)
            || (this->config_.catalog_client_version != updated_config.catalog_client_version)
            || (this->config_.catalog_client_revision != updated_config.catalog_client_revision)
            || (this->config_.catalog_uid_override != updated_config.catalog_uid_override);
        this->config_ = updated_config;

        // The layout already persists the edited config before invoking this
        // callback, so this branch only needs to invalidate runtime caches and
        // restart the dependent loaders.
        if(language_changed) {
            const std::string preferred_language = shield::platform::SystemStatus::DetectPreferredLanguageTag(this->config_.language);
            this->translator_.Load(preferred_language);
            // The catalog status_message was stored as a pre-translated string
            // in the previous language. Re-derive it from the current runtime
            // state so the hero status card reflects the new language.
            if(this->remote_catalog_loading_) {
                this->remote_catalog_state_.status_message = this->translator_.Get("catalog.statusLoading");
            }
            else if(this->remote_catalog_state_.loaded) {
                this->remote_catalog_state_.status_message = this->translator_.Get("catalog.statusLoaded");
                if(!this->remote_catalog_state_.source_title.empty()) {
                    this->remote_catalog_state_.status_message += " " + this->remote_catalog_state_.source_title;
                }
            }
            else if(this->remote_catalog_state_.configured) {
                this->remote_catalog_state_.status_message = this->applet_mode_
                    ? this->translator_.Get("catalog.statusAppletBlocked")
                    : this->translator_.Get("catalog.statusDeferred");
            }
            else {
                this->remote_catalog_state_.status_message = this->translator_.Get("catalog.statusNotConfigured");
            }
        }

        if(remote_catalog_changed) {
            const bool catalog_fetch_in_flight = this->remote_catalog_loading_ && this->remote_catalog_future_.valid();
            if(catalog_fetch_in_flight) {
                shield::platform::RuntimeLog("[catalog] config changed while fetch is active; canceling previous request");
                this->remote_catalog_cancel_requested_ = true;
            }
            this->remote_catalog_state_.catalog = {};
            this->remote_catalog_state_.loaded = false;
            this->remote_catalog_state_.configured = !this->config_.catalog_url.empty();
            this->remote_catalog_state_.source_url = this->config_.catalog_url;
            this->remote_catalog_state_.source_title = this->config_.catalog_title;
            if(this->applet_mode_ && this->remote_catalog_state_.configured) {
                this->remote_catalog_state_.status_message = this->translator_.Get("catalog.statusAppletBlocked");
                this->remote_catalog_loaded_ = true;
            } else {
                this->remote_catalog_state_.status_message = this->remote_catalog_state_.configured
                    ? this->translator_.Get("catalog.statusDeferred")
                    : this->translator_.Get("catalog.statusNotConfigured");
                this->remote_catalog_loaded_ = !this->remote_catalog_state_.configured;
            }
            this->remote_catalog_state_.warning_message.clear();
            this->remote_catalog_state_.update_candidates.clear();
            this->remote_catalog_fail_count_ = 0;
            this->remote_catalog_loading_ = catalog_fetch_in_flight;
            this->remote_catalog_delay_frames_ = 0;
            this->remote_catalog_request_language_.clear();
            this->catalog_warning_pending_ = false;
            this->catalog_warning_shown_ = false;
            this->catalog_warning_delay_frames_ = 0;
            this->startup_catalog_gate_released_ = !this->IsStartupCatalogGateRequired() || this->remote_catalog_loaded_;
        }

        this->layout_->ApplyConfig(this->config_, this->translator_, this->remote_catalog_state_);
        this->UpdateStartupCatalogGate();
        if(update_url_changed) {
            this->app_update_check_started_ = false;
            this->app_update_check_completed_ = false;
            this->app_update_prompt_shown_ = false;
            this->app_update_check_fail_count_ = 0;
            this->app_update_install_requested_ = false;
            this->pending_app_update_ = {};
        }
    });
    this->layout_->SetQueueActionCallback([this](const shield::catalog::QueueItem &item) {
        this->EnqueueItem(item);
    });
    this->layout_->SetQueueBulkActionCallback([this](const std::vector<shield::catalog::QueueItem> &items) {
        if(items.empty()) {
            return;
        }

        // Confirm before mass-queuing. The user triggered this via the "Add all"
        // (X) shortcut on Updates/DLCs; without a confirmation the action is
        // silent and looks like nothing happened.
        const std::string count_line = std::to_string(items.size()) + " "
            + this->translator_.Get("dialog.queueBulkItemsLabel");
        const auto confirm_result = this->ShowThemedDialog(
            this->translator_.Get("dialog.queueTitle"),
            this->translator_.Get("dialog.queueBulkConfirmBody") + "\n\n" + count_line,
            {
                this->translator_.Get("dialog.installOptionsConfirm"),
                this->translator_.Get("dialog.installOptionsCancel")
            },
            true);
        if(confirm_result != 0) {
            return;
        }

        const std::size_t before = this->queue_items_.size();
        this->EnqueueItems(items, false);
        const std::size_t after = this->queue_items_.size();
        const std::size_t added = (after > before) ? (after - before) : 0;

        this->ShowThemedDialog(
            this->translator_.Get("dialog.queueTitle"),
            this->translator_.Get("dialog.queueBulkAdded") + " (" + std::to_string(added) + ")",
            { this->translator_.Get("common.ok") },
            true);
    });
    this->layout_->SetQueueControlCallback([this](const QueueControlAction action, const std::string &title_id) {
        this->RunQueueControlAction(action, title_id);
    });
    this->layout_->SetForwarderInstallCallback([this]() {
        if(shield::platform::ForwarderInstaller::IsInstalled()) {
            if(this->layout_ != nullptr) {
                this->layout_->SetForwarderActivated(true);
            }
            return;
        }

        if(this->forwarder_install_running_ || this->queue_download_loading_) {
            this->ShowThemedDialog(
                this->translator_.Get("forwarder.dialog.title"),
                this->translator_.Get("forwarder.dialog.busyBody"),
                { this->translator_.Get("common.ok") });
            return;
        }

        const auto result = this->ShowThemedDialog(
            this->translator_.Get("forwarder.dialog.title"),
            this->translator_.Get("forwarder.dialog.confirmBody"),
            {
                this->translator_.Get("forwarder.dialog.confirmYes"),
                this->translator_.Get("forwarder.dialog.confirmNo")
            },
            true);
        if(result == 0) {
            this->BeginForwarderInstall();
        }
    });
    this->LoadLayout(this->layout_);
    this->UpdateStartupCatalogGate();
    this->observed_section_ = this->layout_->GetSelectedSection();
    this->QueueAppletModeNotice();

    if(this->layout_ != nullptr) {
        this->layout_->SetForwarderActivated(shield::platform::ForwarderInstaller::IsInstalled());
    }
    this->AddRenderCallback([this]() {
        this->render_frame_count_++;
        if(this->layout_ != nullptr) {
            if(this->layout_->GetSelectedSection() == shield::app::AppSection::Settings && (this->render_frame_count_ % 120) == 0) {
                this->layout_->SetForwarderActivated(shield::platform::ForwarderInstaller::IsInstalled());
            }
            const auto current_section = this->layout_->GetSelectedSection();
            if(current_section != this->observed_section_) {
                this->observed_section_ = current_section;
            }
        }
        this->MaybeFinishRemoteCatalogLoad();
        this->MaybeFinishQueueDownload();
        this->MaybeFinishForwarderInstall();
        this->MaybeFinishAppUpdateCheck();
        this->MaybeFinishAppUpdateInstall();
        this->MaybePushTaskProgressToLayout();
        if(!this->data_loaded_) {
            if(this->render_frame_count_ > 12) {
                this->AdvanceLoadingStage();
            }
        }
        else {
            if(!this->remote_catalog_loaded_ && !this->remote_catalog_loading_) {
                this->remote_catalog_delay_frames_++;
            }
            if(this->remote_catalog_delay_frames_ >= 18) {
                this->MaybeLoadRemoteCatalog();
            }
            if(this->IsInitialUiReadyForAppUpdateCheck()) {
                this->MaybeStartAppUpdateCheck();
            }
            this->MaybeStartQueueDownload();
        }

        this->UpdateStartupCatalogGate();
        this->RefreshAutoSleepPolicy();
        if(this->shutdown_requested_ && !this->shutdown_close_issued_) {
            this->shutdown_exit_screen_frames_++;
            if((this->shutdown_exit_screen_frames_ >= 6) && !this->shutdown_exit_screen_render_logged_) {
                this->shutdown_exit_screen_render_logged_ = true;
                shield::platform::ExitLog("Render: exit screen rendered");
            }
        }
        this->MaybeCompleteShutdown();

        if(!this->shutdown_requested_ && this->catalog_warning_pending_ && !this->catalog_warning_shown_ && !this->remote_catalog_state_.warning_message.empty()) {
            this->catalog_warning_delay_frames_++;
            if(this->catalog_warning_delay_frames_ >= 45) {
                this->catalog_warning_shown_ = true;
                this->catalog_warning_pending_ = false;
                this->ShowThemedDialog(
                    this->translator_.Get("catalog.warningTitle"),
                    this->remote_catalog_state_.warning_message,
                    { this->translator_.Get("common.ok") },
                    true);
            }
        }

        if(!this->shutdown_requested_ && this->applet_mode_notice_pending_ && !this->applet_mode_notice_shown_) {
            this->applet_mode_notice_delay_frames_++;
            if(this->applet_mode_notice_delay_frames_ >= 24) {
                this->applet_mode_notice_shown_ = true;
                this->applet_mode_notice_pending_ = false;
                this->ShowThemedDialog(
                    this->translator_.Get("dialog.appletModeTitle"),
                    this->translator_.Get("dialog.appletModeBody"),
                    { this->translator_.Get("common.ok") },
                    true);
            }
        }
    });

    this->SetOnInput([this](const u64 keys_down, const u64, const u64, const pu::ui::TouchPoint) {
        if((keys_down & HidNpadButton_Plus) || (keys_down & HidNpadButton_Minus)) {
            shield::platform::ExitLog("Input: Plus/Minus pressed, calling ConfirmExitWhileQueueActive()");
            const bool confirmed = this->ConfirmExitWhileQueueActive();
            shield::platform::ExitLog("Input: ConfirmExitWhileQueueActive returned %d", static_cast<int>(confirmed));
            if(confirmed) {
                this->BeginShutdown();
            }
        }
    });
}

}
