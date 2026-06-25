#include <switch.h>

#include <atomic>

#include <install/es_ipc.h>
#include <install/ns_ext_ipc.h>
#include <platform/CurlRuntime.hpp>
#include <platform/ExitLog.hpp>
#include <ui/MainApplication.hpp>

extern "C" {

static bool g_socket_initialized = false;
static bool g_nifm_initialized = false;
static bool g_ncm_initialized = false;
static bool g_nsext_initialized = false;
static bool g_es_initialized = false;
static bool g_spl_initialized = false;
static bool g_spl_crypto_initialized = false;

static constexpr SocketInitConfig kSocketConfig = {
    .tcp_tx_buf_size     = 1024 * 64,
    .tcp_rx_buf_size     = 1024 * 64,
    .tcp_tx_buf_max_size = 1024 * 1024 * 4,
    .tcp_rx_buf_max_size = 1024 * 1024 * 4,
    .udp_tx_buf_size     = 0x2400,
    .udp_rx_buf_size     = 0xA500,
    .sb_efficiency       = 4,
    .num_bsd_sessions    = 3,
    .bsd_service_type    = BsdServiceType_Auto,
};

void userAppInit() {
    shield::platform::ExitLog("userAppInit: build_marker=full-memory-nifm-exit-v2");
    shield::platform::ExitLog("userAppInit: entry applet_type=%d", static_cast<int>(appletGetAppletType()));

    Result rc = socketInitialize(&kSocketConfig);
    g_socket_initialized = R_SUCCEEDED(rc);
    shield::platform::ExitLog("userAppInit: socketInitialize rc=0x%x", rc);

    rc = nifmInitialize(NifmServiceType_User);
    g_nifm_initialized = R_SUCCEEDED(rc);
    shield::platform::ExitLog("userAppInit: nifmInitialize rc=0x%x", rc);

    rc = ncmInitialize();
    g_ncm_initialized = R_SUCCEEDED(rc);
    shield::platform::ExitLog("userAppInit: ncmInitialize rc=0x%x", rc);

    rc = nsextInitialize();
    g_nsext_initialized = R_SUCCEEDED(rc);
    shield::platform::ExitLog("userAppInit: nsextInitialize rc=0x%x", rc);

    rc = esInitialize();
    g_es_initialized = R_SUCCEEDED(rc);
    shield::platform::ExitLog("userAppInit: esInitialize rc=0x%x", rc);

    rc = splInitialize();
    g_spl_initialized = R_SUCCEEDED(rc);
    shield::platform::ExitLog("userAppInit: splInitialize rc=0x%x", rc);

    rc = splCryptoInitialize();
    g_spl_crypto_initialized = R_SUCCEEDED(rc);
    shield::platform::ExitLog("userAppInit: splCryptoInitialize rc=0x%x", rc);
}

void userAppExit() {
    // libnx can re-enter userAppExit through the atexit/fini chain. Service
    // finalizers are not idempotent, so a second pass must return immediately.
    static std::atomic<bool> already_exiting{false};
    if(already_exiting.exchange(true)) {
        shield::platform::ExitLog("userAppExit: re-entry detected, returning");
        return;
    }

    shield::platform::ExitLog("userAppExit: entry applet_type=%d", static_cast<int>(appletGetAppletType()));
    appletSetAutoSleepDisabled(false);

    if(g_spl_crypto_initialized) {
        shield::platform::ExitLog("userAppExit: before splCryptoExit");
        splCryptoExit();
        g_spl_crypto_initialized = false;
    }
    if(g_spl_initialized) {
        shield::platform::ExitLog("userAppExit: before splExit");
        splExit();
        g_spl_initialized = false;
    }
    if(g_es_initialized) {
        shield::platform::ExitLog("userAppExit: before esExit");
        esExit();
        g_es_initialized = false;
    }
    if(g_nsext_initialized) {
        shield::platform::ExitLog("userAppExit: before nsextExit");
        nsextExit();
        g_nsext_initialized = false;
    }
    if(g_ncm_initialized) {
        shield::platform::ExitLog("userAppExit: before ncmExit");
        ncmExit();
        g_ncm_initialized = false;
    }

    // Drop our bookkeeping for reusable curl handles before process exit. The
    // handles themselves are intentionally left for process teardown because
    // curl_easy_cleanup has been observed to crash with tainted TLS state.
    shield::platform::ExitLog("userAppExit: before ShutdownCurl");
    shield::platform::ShutdownCurl();

    // Curl disables keep-alive on every transfer, so BSD should not have live
    // curl connections by this point. Close socket/NIFM normally so every host
    // sees a balanced teardown.
    if(g_socket_initialized || g_nifm_initialized) {
        if(g_socket_initialized) {
            shield::platform::ExitLog("userAppExit: before socketExit");
            socketExit();
            g_socket_initialized = false;
            shield::platform::ExitLog("userAppExit: socketExit returned");
        }
        if(g_nifm_initialized) {
            shield::platform::ExitLog("userAppExit: before nifmExit");
            nifmExit();
            g_nifm_initialized = false;
            shield::platform::ExitLog("userAppExit: nifmExit returned");
        }
    }

    shield::platform::ExitLog("userAppExit: exit (service teardown complete)");
}

}

int main() {
    // Configure the renderer with RomFS and controller support. The lightweight
    // shell avoids image decoding entirely and renders a text-first catalog.
    auto renderer_options = pu::ui::render::RendererInitOptions(SDL_INIT_EVERYTHING, pu::ui::render::RendererHardwareFlags);
    renderer_options.UseRomfs();
    renderer_options.SetPlServiceType(PlServiceType_User);
    renderer_options.AddDefaultAllSharedFonts();
    // Extra compact font used by the top-bar chips (storage, network, clock).
    renderer_options.AddExtraDefaultFontSize(22);
    // Smaller compact font used by listing cards and footer status text.
    renderer_options.AddExtraDefaultFontSize(20);
    renderer_options.SetInputPlayerCount(1);
    renderer_options.AddInputNpadStyleTag(HidNpadStyleSet_NpadStandard);
    renderer_options.AddInputNpadIdType(HidNpadIdType_Handheld);
    renderer_options.AddInputNpadIdType(HidNpadIdType_No1);

    auto renderer = pu::ui::render::Renderer::New(renderer_options);
    auto application = shield::ui::MainApplication::New(renderer);

    // Load the application before trying to render anything on-screen.
    const Result rc = application->Load();
    if(R_FAILED(rc)) {
        diagAbortWithResult(rc);
    }

    application->ShowWithFadeIn();
    return 0;
}
