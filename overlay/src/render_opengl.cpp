#include "internal.hpp"

#if STRATA_HAS_OPENGL

#include <tlhelp32.h>
#include <GL/gl.h>

namespace strata::overlay::detail {

namespace {

using swap_buffers_fn = BOOL(WINAPI*)(HDC);

thread_local bool t_in_gl_swap = false;

[[nodiscard]] bool iequals(const char* a, const char* b) noexcept
{
    if (a == nullptr || b == nullptr) { return false; }
    while (*a != '\0' && *b != '\0') {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') { ca = static_cast<char>(ca - 'A' + 'a'); }
        if (cb >= 'A' && cb <= 'Z') { cb = static_cast<char>(cb - 'A' + 'a'); }
        if (ca != cb) { return false; }
    }
    return *a == '\0' && *b == '\0';
}

BOOL WINAPI hook_swap_buffers_gdi(HDC hdc)
{
    struct hook_scope {
        hook_scope() { g.in_hook.fetch_add(1); }
        ~hook_scope() { g.in_hook.fetch_sub(1); }
    } const scope;

    const auto orig = reinterpret_cast<swap_buffers_fn>(g.gl_orig_swap_gdi.load(std::memory_order_acquire));
    if (!g.shutting_down.load() && !t_in_gl_swap) {
        t_in_gl_swap = true;
        on_gl_swap(hdc);
        t_in_gl_swap = false;
    }
    return orig != nullptr ? orig(hdc) : ::SwapBuffers(hdc);
}

BOOL WINAPI hook_wgl_swap_buffers(HDC hdc)
{
    struct hook_scope {
        hook_scope() { g.in_hook.fetch_add(1); }
        ~hook_scope() { g.in_hook.fetch_sub(1); }
    } const scope;

    const auto orig = reinterpret_cast<swap_buffers_fn>(g.gl_orig_swap_wgl.load(std::memory_order_acquire));
    if (!g.shutting_down.load() && !t_in_gl_swap) {
        t_in_gl_swap = true;
        on_gl_swap(hdc);
        t_in_gl_swap = false;
    }
    return orig != nullptr ? orig(hdc) : FALSE;
}

void patch_module_iat(HMODULE mod, const char* dll_name, const char* func_name, void* hook, std::atomic<void*>& orig_slot)
{
    if (mod == nullptr) { return; }
    auto* base = reinterpret_cast<const unsigned char*>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { return; }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { return; }

    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) { return; }

    const auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; imp->Name != 0; ++imp) {
        const char* mod_name = reinterpret_cast<const char*>(base + imp->Name);
        if (!iequals(mod_name, dll_name)) { continue; }
        const DWORD orig_rva = imp->OriginalFirstThunk != 0 ? imp->OriginalFirstThunk : imp->FirstThunk;
        if (orig_rva == 0 || imp->FirstThunk == 0) { continue; }
        const auto* orig_thunk = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + orig_rva);
        auto*       iat_thunk  = reinterpret_cast<IMAGE_THUNK_DATA*>(const_cast<unsigned char*>(base) + imp->FirstThunk);
        for (; orig_thunk->u1.AddressOfData != 0; ++orig_thunk, ++iat_thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(orig_thunk->u1.Ordinal)) { continue; }
            const auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + orig_thunk->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(ibn->Name), func_name) != 0) { continue; }

            void** slot = reinterpret_cast<void**>(&iat_thunk->u1.Function);
            if (*slot == hook) { continue; }
            DWORD old_prot = 0;
            if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_prot)) { continue; }
            void* orig = *slot;
            void* expected = nullptr;
            orig_slot.compare_exchange_strong(expected, orig, std::memory_order_release);
            {
                const std::lock_guard lk{g.gl_iat_mutex};
                g.gl_iat_patches.push_back({slot, hook, orig, true});
            }
            *slot = hook;
            DWORD ignored = 0;
            ::VirtualProtect(slot, sizeof(void*), old_prot, &ignored);
        }
    }
}

void apply_pending_scale_gl()
{
    const float wanted = g.want_scale.exchange(0.0f);
    if (wanted <= 0.0f || g.ui == nullptr || g.renderer_gl == nullptr) { return; }
    const u32 before = g.ui->font_generation();
    if (!g.ui->set_scale(wanted) || g.ui->font_generation() == before) { return; }
    if (!g.renderer_gl->update_atlas(g.ui->font())) {
        log_line("apply_pending_scale_gl: renderer could not take new atlas");
    }
    g.ui->release_font_pixels();
}

} // namespace

bool install_gl_hooks()
{
    HMODULE self_mod = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&install_gl_hooks), &self_mod);

    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, ::GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) {
        patch_module_iat(::GetModuleHandleW(nullptr), "gdi32.dll", "SwapBuffers", reinterpret_cast<void*>(&hook_swap_buffers_gdi), g.gl_orig_swap_gdi);
        patch_module_iat(::GetModuleHandleW(nullptr), "opengl32.dll", "wglSwapBuffers", reinterpret_cast<void*>(&hook_wgl_swap_buffers), g.gl_orig_swap_wgl);
    } else {
        MODULEENTRY32W me{};
        me.dwSize = sizeof me;
        for (BOOL ok = ::Module32FirstW(snap, &me); ok; ok = ::Module32NextW(snap, &me)) {
            if (me.hModule == self_mod) { continue; }
            patch_module_iat(me.hModule, "gdi32.dll", "SwapBuffers", reinterpret_cast<void*>(&hook_swap_buffers_gdi), g.gl_orig_swap_gdi);
            patch_module_iat(me.hModule, "opengl32.dll", "wglSwapBuffers", reinterpret_cast<void*>(&hook_wgl_swap_buffers), g.gl_orig_swap_wgl);
        }
        ::CloseHandle(snap);
    }

    std::size_t count = 0;
    {
        const std::lock_guard lk{g.gl_iat_mutex};
        count = g.gl_iat_patches.size();
    }
    log_line("opengl iat hooks installed: %zu slots", count);
    if (count == 0) {
        set_error("could not find SwapBuffers / wglSwapBuffers IAT entries to hook");
        return false;
    }
    return true;
}

bool remove_gl_hooks()
{
    const std::lock_guard lk{g.gl_iat_mutex};
    bool all = true;
    for (auto& p : g.gl_iat_patches) {
        if (!p.patched) { continue; }
        if (*p.slot != p.hook) {
            all = false;
            continue;
        }
        DWORD old_prot = 0;
        if (!::VirtualProtect(p.slot, sizeof(void*), PAGE_READWRITE, &old_prot)) {
            all = false;
            continue;
        }
        *p.slot = p.orig;
        DWORD ignored = 0;
        ::VirtualProtect(p.slot, sizeof(void*), old_prot, &ignored);
        p.patched = false;
    }
    if (all) { g.gl_iat_patches.clear(); }
    return all;
}

void release_gl_objects() noexcept
{
    if (g.renderer_gl != nullptr) {
        if (::wglGetCurrentContext() == g.gl_rc && g.gl_rc != nullptr) {
            g.renderer_gl->destroy();
            g.renderer_gl.reset();
        } else {
            // no matching GL context on this thread: detach without calling glDelete* against the wrong/null context
            (void)g.renderer_gl.release();
        }
    }
    g.gl_rc = nullptr;
}

void on_gl_swap(HDC hdc)
{
    if (hdc == nullptr || g.attach_failed) { return; }
    HGLRC rc = ::wglGetCurrentContext();
    if (rc == nullptr) { return; }
    HWND hwnd = ::WindowFromDC(hdc);
    if (hwnd == nullptr || !::IsWindow(hwnd)) { return; }

    const bool first_time = g.ui == nullptr;
    if (first_time) {
        context_config cfg;
        if (g.opt.configure_context) { g.opt.configure_context(cfg); }
        auto created = context::create(cfg);
        if (!created) {
            set_error("could not build the font atlas");
            g.attach_failed = true;
            return;
        }
        g.ui = std::make_unique<context>(std::move(*created));
        g.platform.attach(hwnd);
        g.base_scale = g.opt.ui_scale > 0.0f ? g.opt.ui_scale : g.platform.dpi_scale();
        (void)g.ui->set_scale(g.base_scale);
        g.want_scale.store(0.0f);
    }

    if (g.backend != state::api::opengl || g.gl_rc != rc || g.renderer_gl == nullptr || !g.renderer_gl->valid()) {
        if (!first_time) { log_line("opengl: new context %p", static_cast<void*>(rc)); }
        if (g.renderer_gl != nullptr) {
            if (g.gl_rc == rc) {
                g.renderer_gl->destroy();
                g.renderer_gl.reset();
            } else if (g.gl_rc != nullptr && ::wglMakeCurrent(hdc, g.gl_rc)) {
                g.renderer_gl->destroy();
                g.renderer_gl.reset();
                ::wglMakeCurrent(hdc, rc);
            } else {
                // old context cannot be made current: avoid deleting its handle IDs inside the new context
                (void)g.renderer_gl.release();
            }
        }
        if (!ensure_font_pixels()) { g.attach_failed = true; return; }
        g.backend = state::api::opengl;
        g.gl_rc   = rc;
        auto fresh = std::make_unique<gl_renderer>();
        if (!fresh->create(g.ui->font())) {
            set_error("could not create the opengl ui renderer");
            g.attach_failed = true;
            return;
        }
        fresh->set_state_restore(true);
        g.renderer_gl = std::move(fresh);
        g.ui->release_font_pixels();
        if (g.opt.output.has_value()) {
            output_desc od;
            od.space            = *g.opt.output;
            od.paper_white_nits = g.opt.hdr_paper_white_nits;
            g.renderer_gl->set_output(od);
            g.output_now.store(static_cast<int>(od.space));
        }
        subclass_window(hwnd);
        if (first_time && g.opt.on_ready) { g.opt.on_ready(*g.ui); }
        if (g.visible.load() && g.hwnd != nullptr) { ::PostMessageW(g.hwnd, wm_show_changed, 0, 0); }
        log_line("attached (opengl): window %p, subclassed %d", static_cast<void*>(g.hwnd), g.subclassed ? 1 : 0);
    }

    if (!g.subclassed || !(g.visible.load() || g.draw_hidden.load())) { return; }

    apply_pending_scale_gl();

    const bool open = g.visible.load();
    input_state input;
    {
        const std::lock_guard lock{g.platform_mutex};
        input = g.platform.new_frame();
    }
    if (!open) {
        input_state idle;
        idle.display_size = input.display_size;
        idle.delta_time   = input.delta_time;
        input = std::move(idle);
    }
    {
        std::vector<std::function<void()>> todo;
        { const std::lock_guard lock{g.post_mutex}; todo.swap(g.posted); }
        for (auto& fn : todo) { fn(); }
    }

    g.ui->begin_frame(std::move(input));
    if (open && g.opt.scale_hotkeys && !g.ui->want_text_input()) {
        const auto stepped = [&](int direction) {
            const float next = std::clamp(g.ui->scale() + 0.1f * static_cast<float>(direction), 0.5f, 4.0f);
            g.want_scale.store(next);
        };
        if (g.ui->key_pressed(strata::key::equals, true) || g.ui->key_pressed(strata::key::num_add, true)) { stepped(1); }
        else if (g.ui->key_pressed(strata::key::minus, true) || g.ui->key_pressed(strata::key::num_subtract, true)) { stepped(-1); }
        else if (g.ui->key_pressed(strata::key::d0, true) || g.ui->key_pressed(strata::key::num0, true)) { g.want_scale.store(g.base_scale); }
    }
    if (g.opt.hud) { g.opt.hud(*g.ui); }
    if (open && g.opt.ui) { g.opt.ui(*g.ui); }
    g.ui->end_frame();

    g.capture_mouse.store(open && g.ui->want_capture_mouse());
    g.capture_keys.store(open && g.ui->want_text_input());
    if (open && (g.opt.block_game_input || g.capture_mouse.load())) {
        {
            const std::lock_guard lock{g.platform_mutex};
            g.platform.set_cursor(g.ui->cursor());
        }
        ::ClipCursor(nullptr);
    }

    // draw into the window's default framebuffer even if the host left an offscreen FBO bound
    using bind_fbo_fn = void(WINAPI*)(GLenum, GLuint);
    const auto bind_fbo = reinterpret_cast<bind_fbo_fn>(reinterpret_cast<void*>(::wglGetProcAddress("glBindFramebuffer")));
    GLint prev_draw_fbo = 0;
    if (bind_fbo != nullptr) {
        constexpr GLenum kGL_DRAW_FRAMEBUFFER_BINDING = 0x8CA6;
        constexpr GLenum kGL_DRAW_FRAMEBUFFER         = 0x8CA9;
        ::glGetIntegerv(kGL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fbo);
        if (prev_draw_fbo != 0) { bind_fbo(kGL_DRAW_FRAMEBUFFER, 0); }
    }
    g.renderer_gl->render(g.ui->render_data());
    if (bind_fbo != nullptr && prev_draw_fbo != 0) {
        constexpr GLenum kGL_DRAW_FRAMEBUFFER = 0x8CA9;
        bind_fbo(kGL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prev_draw_fbo));
    }
    g.frame_count.fetch_add(1);
    ++g.visible_frames;
}

} // namespace strata::overlay::detail

#endif // STRATA_HAS_OPENGL
