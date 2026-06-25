#pragma once

#include <array>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <pu/sdl2/sdl2_Types.hpp>
#include <pu/ui/elm/elm_Image.hpp>
#include <pu/ui/elm/elm_Rectangle.hpp>
#include <pu/ui/elm/elm_TextBlock.hpp>
#include <pu/ui/ui_Layout.hpp>

#include <app/AppConfig.hpp>
#include <app/AppSection.hpp>
#include <catalog/InstalledTitle.hpp>
#include <catalog/QueueItem.hpp>
#include <catalog/RemoteCatalogState.hpp>
#include <i18n/I18n.hpp>
#include <platform/SystemStatus.hpp>

namespace shield::ui {

enum class QueueControlAction : std::uint8_t {
    PauseResume = 0,
    PauseResumeAll,
    Cancel,
    CancelAll,
    ClearItem,
    ClearAll,
    MoveUp,
    MoveDown
};

struct TaskProgress {
    bool active = false;
    std::string title;
    std::string detail;
    float ratio = 0.0f;
    std::string active_title_id;
    std::uint64_t bytes_done = 0;
    std::uint64_t bytes_total = 0;
    double bytes_per_second = 0.0;
    bool is_installing = false;
};

struct ShellCardContent {
    std::string badge;
    std::string title;
    std::string subtitle;
    std::string footer;
    std::string image_path;
    pu::ui::Color color;

    ShellCardContent() = default;

    ShellCardContent(std::string badge, std::string title, std::string subtitle, std::string footer, pu::ui::Color color, std::string image_path = {})
        : badge(std::move(badge)), title(std::move(title)), subtitle(std::move(subtitle)), footer(std::move(footer)), image_path(std::move(image_path)), color(color) {
    }
};

class ShellLayout : public pu::ui::Layout {
    private:
        enum class FocusZone : std::uint8_t {
            Sidebar = 0,
            Cards
        };

        struct NavWidgets {
            pu::ui::elm::Rectangle::Ref selection_background;
            pu::ui::elm::Rectangle::Ref accent_bar;
            pu::ui::elm::TextBlock::Ref label;
        };

        struct CardWidgets {
            pu::ui::elm::Rectangle::Ref focus_outline;
            pu::ui::elm::Rectangle::Ref surface;
            pu::ui::elm::Rectangle::Ref badge_background;
            pu::ui::elm::TextBlock::Ref badge;
            pu::ui::elm::TextBlock::Ref title;
            pu::ui::elm::TextBlock::Ref subtitle;
            pu::ui::elm::TextBlock::Ref footer;
        };

        struct StorageWidgets {
            pu::ui::elm::TextBlock::Ref label;
            pu::ui::elm::Rectangle::Ref track;
            pu::ui::elm::Rectangle::Ref fill;
            pu::ui::elm::TextBlock::Ref value;
            std::string label_key;
        };

        shield::app::AppConfig config_;
        shield::i18n::I18n translator_;
        std::vector<shield::catalog::InstalledTitle> installed_titles_;
        std::vector<shield::catalog::QueueItem> queue_items_;
        shield::catalog::RemoteCatalogState remote_catalog_state_;
        shield::platform::SystemOverview system_overview_;
        TaskProgress task_progress_;
        bool applet_mode_ = false;
        shield::app::AppSection selected_section_;
        FocusZone focus_zone_ = FocusZone::Sidebar;
        std::size_t selected_card_index_ = 0;
        std::size_t card_view_offset_ = 0;
        bool touch_active_ = false;
        bool touch_moved_ = false;
        int touch_start_x_ = 0;
        int touch_start_y_ = 0;
        int touch_scroll_anchor_y_ = 0;
        std::function<void(const std::string &, const std::string &)> dialog_callback_;
        std::function<void(const shield::app::AppConfig &)> config_changed_callback_;
        std::function<void(const shield::catalog::QueueItem &)> queue_action_callback_;
        std::function<void(const std::vector<shield::catalog::QueueItem> &)> queue_bulk_action_callback_;
        std::function<void(QueueControlAction, const std::string &)> queue_control_callback_;
        std::function<void()> forwarder_install_callback_;
        bool forwarder_activated_ = false;
        std::string clock_cache_;
        std::string search_query_;
        std::array<std::string, shield::app::kAllSections.size()> quick_filters_ = {};
        std::array<std::string, 10> card_dialog_titles_ = {};
        std::array<std::string, 10> card_dialog_bodies_ = {};
        std::vector<ShellCardContent> cached_cards_;
        std::vector<shield::catalog::RemoteTitleMetadata> cached_remote_titles_;
        std::vector<shield::catalog::RemoteTitleMetadata> cached_recommended_titles_;
        std::vector<shield::catalog::RemoteTitleMetadata> cached_new_game_titles_;
        std::vector<shield::catalog::RemoteTitleMetadata> cached_dlc_titles_;
        std::vector<shield::catalog::InstalledTitle> cached_sorted_installed_titles_;
        std::vector<shield::catalog::UpdateCandidate> cached_sorted_update_candidates_;
        std::vector<shield::catalog::InstalledTitle> cached_installed_titles_;
        std::vector<shield::catalog::UpdateCandidate> cached_update_candidates_;
        std::vector<shield::catalog::QueueItem> cached_queue_items_;
        bool queue_detail_mode_ = false;
        bool shutting_down_ = false;
        bool startup_gate_active_ = false;
        std::string startup_gate_title_;
        std::string startup_gate_detail_;
        float startup_gate_ratio_ = 0.0f;
        bool startup_gate_show_progress_ = true;
        std::uint32_t nav_hold_frames_ = 0;
        bool remote_section_cache_valid_ = false;
        bool local_section_cache_valid_ = false;
        std::unordered_map<std::string, pu::sdl2::TextureHandle::Ref> image_texture_cache_;
        pu::ui::elm::Rectangle::Ref sidebar_background_;
        pu::ui::elm::Rectangle::Ref sidebar_brand_background_;
        pu::ui::elm::Rectangle::Ref top_bar_;
        pu::ui::elm::Rectangle::Ref hero_background_;
        pu::ui::elm::Rectangle::Ref hero_badge_background_;
        pu::ui::elm::Rectangle::Ref status_card_bg_;
        pu::ui::elm::TextBlock::Ref app_title_;
        pu::ui::elm::TextBlock::Ref app_subtitle_;
        pu::ui::elm::TextBlock::Ref sidebar_status_label_;
        pu::ui::elm::TextBlock::Ref header_title_;
        pu::ui::elm::TextBlock::Ref header_subtitle_;
        pu::ui::elm::TextBlock::Ref header_clock_;
        pu::ui::elm::TextBlock::Ref header_network_label_;
        pu::ui::elm::TextBlock::Ref hero_badge_;
        pu::ui::elm::TextBlock::Ref hero_title_;
        pu::ui::elm::TextBlock::Ref hero_subtitle_;
        pu::ui::elm::TextBlock::Ref hero_body_;
        pu::ui::elm::Rectangle::Ref task_progress_track_;
        pu::ui::elm::Rectangle::Ref task_progress_fill_;
        pu::ui::elm::TextBlock::Ref task_progress_title_;
        pu::ui::elm::TextBlock::Ref task_progress_detail_;
        pu::ui::elm::TextBlock::Ref footer_hints_;
        pu::ui::elm::Rectangle::Ref queue_detail_panel_;
        pu::ui::elm::Rectangle::Ref queue_detail_badge_background_;
        pu::ui::elm::TextBlock::Ref queue_detail_badge_;
        pu::ui::elm::Rectangle::Ref queue_detail_artwork_background_;
        pu::ui::elm::Image::Ref queue_detail_artwork_;
        pu::ui::elm::TextBlock::Ref queue_detail_title_;
        pu::ui::elm::TextBlock::Ref queue_detail_subtitle_;
        pu::ui::elm::TextBlock::Ref queue_detail_status_;
        pu::ui::elm::Rectangle::Ref queue_detail_progress_track_;
        pu::ui::elm::Rectangle::Ref queue_detail_progress_fill_;
        pu::ui::elm::TextBlock::Ref queue_detail_progress_;
        pu::ui::elm::TextBlock::Ref queue_detail_primary_action_;
        pu::ui::elm::TextBlock::Ref queue_detail_secondary_action_;
        pu::ui::elm::TextBlock::Ref queue_detail_body_;
        pu::ui::elm::Rectangle::Ref startup_gate_background_;
        pu::ui::elm::Rectangle::Ref startup_gate_sidebar_background_;
        pu::ui::elm::TextBlock::Ref startup_gate_title_text_;
        pu::ui::elm::TextBlock::Ref startup_gate_detail_text_;
        pu::ui::elm::Rectangle::Ref startup_gate_progress_track_;
        pu::ui::elm::Rectangle::Ref startup_gate_progress_fill_;
        StorageWidgets nand_widgets_;
        StorageWidgets sd_widgets_;
        std::array<pu::ui::elm::Rectangle::Ref, 4> network_bars_;
        pu::ui::elm::Rectangle::Ref network_dot_;
        std::array<pu::ui::elm::Rectangle::Ref, 6> ethernet_icon_segments_;
        std::array<NavWidgets, shield::app::kAllSections.size()> nav_widgets_;
        std::array<CardWidgets, 10> card_widgets_;

        void BuildStaticLayout();
        void RefreshAll();
        void RefreshNavigation();
        void RefreshHeader();
        void RefreshSystemOverview();
        void RefreshHero();
        void RefreshCards();
        void RefreshQueueDetailPage();
        void RefreshTaskProgress();
        void RefreshFooter();
        void RefreshClock();
        void RefreshStartupGate();
        bool IsGridModeEnabled() const;
        void RebuildCardCache();
        void InvalidateSectionCaches();
        void EnsureRemoteSectionCaches();
        void EnsureLocalSectionCaches();
        pu::sdl2::TextureHandle::Ref LoadCardTexture(const std::string &path);
        void PruneImageTextureCache(const std::unordered_set<std::string> &keep_paths);
        std::size_t GetVisibleCardCount() const;
        std::size_t GetCardPageSlotCount() const;
        void EnsureCardSelectionVisible();
        void MoveSelection(int delta);
        void MoveCardSelection(int delta);
        void MoveFocusHorizontal(int delta);
        int GetGridStep() const;
        void ActivateSelection();
        void CycleSortMode();
        void CycleQuickFilter();
        void CycleSettingsOption();
        bool PromptTextInput(const std::string &guide_text, std::string &value, bool password = false, std::size_t max_length = 256) const;
        void EditConnections();
        void ApplyThemePalette();
        void BeginSearch();
        void QueueAllVisibleItems();
        void ToggleQueueDetailMode(bool enabled);
        void RunQueueControlAction(QueueControlAction action);
        void ShowCurrentSelectionInfo();
        bool PersistConfig(bool notify_application = true);
        void HandleTouch(const pu::ui::TouchPoint &touch_pos);
        void SetSelectedSection(shield::app::AppSection section);
        bool TryBuildQueueItemForRemoteTitle(const shield::catalog::RemoteTitleMetadata &title, shield::catalog::QueueItem &out_item, bool include_latest_update, bool include_all_dlcs) const;
        bool TryBuildQueueItemForUpdateCandidate(const shield::catalog::UpdateCandidate &candidate, shield::catalog::QueueItem &out_item) const;
        bool TryBuildQueueItemForSelection(shield::catalog::QueueItem &out_item) const;

    public:
        ShellLayout(const shield::app::AppConfig &config, const shield::i18n::I18n &translator, std::vector<shield::catalog::InstalledTitle> installed_titles, std::vector<shield::catalog::QueueItem> queue_items, const shield::catalog::RemoteCatalogState &remote_catalog_state, const shield::platform::SystemOverview &system_overview, bool applet_mode);
        PU_SMART_CTOR(ShellLayout)

        void SetShuttingDown(bool val);
        void SetDialogCallback(std::function<void(const std::string &, const std::string &)> callback);
        void SetConfigChangedCallback(std::function<void(const shield::app::AppConfig &)> callback);
        void SetQueueActionCallback(std::function<void(const shield::catalog::QueueItem &)> callback);
        void SetQueueBulkActionCallback(std::function<void(const std::vector<shield::catalog::QueueItem> &)> callback);
        void SetQueueControlCallback(std::function<void(QueueControlAction, const std::string &)> callback);
        void SetForwarderInstallCallback(std::function<void()> callback);
        void SetForwarderActivated(bool activated);
        void ApplyStartupGate(bool active, const std::string &title, const std::string &detail, float ratio, bool show_progress = true);
        shield::app::AppSection GetSelectedSection() const;
        bool IsCardFocused() const;
        std::string GetSelectedCardDialogTitle() const;
        std::string GetSelectedCardDialogBody() const;
        void ApplyConfig(const shield::app::AppConfig &config, const shield::i18n::I18n &translator, const shield::catalog::RemoteCatalogState &remote_catalog_state);
        void ApplyLoadedData(const std::vector<shield::catalog::InstalledTitle> &installed_titles, const std::vector<shield::catalog::QueueItem> &queue_items, const shield::catalog::RemoteCatalogState &remote_catalog_state, const shield::platform::SystemOverview &system_overview, bool refresh_remote_catalog = true);
        void ApplyQueueData(const std::vector<shield::catalog::QueueItem> &queue_items);
        void ApplyTaskProgress(const TaskProgress &task_progress);
        void RefreshVisibleCards();
};

}
