#include "strata/platform/win32.hpp"

#include <windows.h>
#include <windowsx.h>
#include <imm.h>

#include "strata/vmem.hpp"

#include <algorithm>
#include <array>
#include <string>

namespace strata {

namespace {

[[nodiscard]] std::int64_t query_ticks() noexcept
{
    LARGE_INTEGER t;
    ::QueryPerformanceCounter(&t);
    return t.QuadPart;
}

[[nodiscard]] std::int64_t tick_frequency() noexcept
{
    static const std::int64_t freq = [] {
        LARGE_INTEGER f;
        ::QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return freq;
}

struct clipboard_scope {
    bool open;
    explicit clipboard_scope(HWND hwnd) noexcept : open{::OpenClipboard(hwnd) != FALSE} {}
    ~clipboard_scope()
    {
        if (open) { ::CloseClipboard(); }
    }
};

void clipboard_set(void* user, std::string_view text) noexcept
{
    const clipboard_scope clip{static_cast<HWND>(user)};
    if (!clip.open) { return; }

    const int wide_len = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, (static_cast<SIZE_T>(wide_len) + 1) * sizeof(wchar_t));
    if (mem == nullptr) { return; }
    auto* dst = static_cast<wchar_t*>(::GlobalLock(mem));
    if (dst == nullptr) {
        ::GlobalFree(mem);
        return;
    }
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), dst, wide_len);
    dst[wide_len] = L'\0';
    ::GlobalUnlock(mem);

    ::EmptyClipboard();
    if (::SetClipboardData(CF_UNICODETEXT, mem) == nullptr) {
        ::GlobalFree(mem); // ownership only transfers on success
    }
}

bool clipboard_get(void* user, std::string& out) noexcept
{
    const clipboard_scope clip{static_cast<HWND>(user)};
    if (!clip.open) { return false; }

    HANDLE data = ::GetClipboardData(CF_UNICODETEXT);
    if (data == nullptr) { return false; }
    const auto* src = static_cast<const wchar_t*>(::GlobalLock(data));
    if (src == nullptr) { return false; }

    const int len = ::WideCharToMultiByte(CP_UTF8, 0, src, -1, nullptr, 0, nullptr, nullptr);
    bool ok = false;
    if (len > 1) {
        out.resize(static_cast<std::size_t>(len - 1));
        ok = ::WideCharToMultiByte(CP_UTF8, 0, src, -1, out.data(), len, nullptr, nullptr) > 0;
    }
    ::GlobalUnlock(data);
    return ok;
}

} // namespace

void win32_platform::attach(void* hwnd) noexcept
{
    hwnd_       = hwnd;
    last_ticks_ = query_ticks();
}

f32 win32_platform::dpi_scale() const noexcept
{
    if (hwnd_ == nullptr) {
        return 1.0f;
    }
    const UINT dpi = ::GetDpiForWindow(static_cast<HWND>(hwnd_));
    return dpi == 0 ? 1.0f : static_cast<f32>(dpi) / 96.0f;
}

bool win32_platform::apply_cursor() noexcept
{
    LPCWSTR shape = IDC_ARROW;
    switch (cursor_) {
    case cursor_kind::text:        shape = IDC_IBEAM; break;
    case cursor_kind::hand:        shape = IDC_HAND; break;
    case cursor_kind::not_allowed: shape = IDC_NO; break;
    case cursor_kind::resize_ew:   shape = IDC_SIZEWE; break;
    case cursor_kind::resize_ns:   shape = IDC_SIZENS; break;
    case cursor_kind::resize_nwse: shape = IDC_SIZENWSE; break;
    case cursor_kind::resize_nesw: shape = IDC_SIZENESW; break;
    default:                       break;
    }
    ::SetCursor(::LoadCursorW(nullptr, shape));
    return true;
}

win32_platform::appearance_settings win32_platform::appearance() noexcept
{
    appearance_settings a;
    DWORD value = 1;
    DWORD size  = sizeof value;
    if (::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                       RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        a.dark = value == 0;
    }
    HIGHCONTRASTW hc{sizeof hc};
    a.high_contrast = ::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) != FALSE && (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    DWORD accent = 0; // 0xAABBGGRR
    size = sizeof accent;
    if (::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr, &accent,
                       &size) == ERROR_SUCCESS) {
        a.accent = {static_cast<u8>(accent), static_cast<u8>(accent >> 8), static_cast<u8>(accent >> 16), 255};
    }
    return a;
}

clipboard_hooks win32_platform::clipboard() noexcept
{
    return {&clipboard_set, &clipboard_get, hwnd_};
}

void win32_platform::push_key(key k, bool ctrl, bool shift, bool alt) noexcept
{
    if (key_count_ < keys_.size()) {
        keys_[key_count_++] = {k, ctrl, shift, alt};
    }
}

void win32_platform::push_press(u32 key) noexcept
{
    pressed_key_ = key;
    if (press_count_ < presses_.size()) {
        presses_[press_count_++] = {key, (::GetKeyState(VK_CONTROL) & 0x8000) != 0, (::GetKeyState(VK_SHIFT) & 0x8000) != 0,
                                    (::GetKeyState(VK_MENU) & 0x8000) != 0};
    }
}

void win32_platform::push_text(char32_t cp) noexcept
{
    char utf8[4];
    u32  len = 0;
    if (cp < 0x80) {
        utf8[len++] = static_cast<char>(cp);
    } else if (cp < 0x800) {
        utf8[len++] = static_cast<char>(0xc0 | (cp >> 6));
        utf8[len++] = static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        utf8[len++] = static_cast<char>(0xe0 | (cp >> 12));
        utf8[len++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        utf8[len++] = static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        utf8[len++] = static_cast<char>(0xf0 | (cp >> 18));
        utf8[len++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        utf8[len++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        utf8[len++] = static_cast<char>(0x80 | (cp & 0x3f));
    }
    if (typed_len_ + len <= typed_.size()) {
        for (u32 i = 0; i < len; ++i) { typed_[typed_len_++] = utf8[i]; }
    }
}

bool win32_platform::handle_message(void* hwnd, std::uint32_t msg, std::uintptr_t wparam, std::intptr_t lparam) noexcept
{
    const auto wnd = static_cast<HWND>(hwnd);

    const auto press = [&](std::size_t button) {
        if (!(down_[0] || down_[1] || down_[2])) {
            ::SetCapture(wnd);
        }
        down_[button]       = true;
        press_seen_[button] = true;
    };
    const auto release = [&](std::size_t button) {
        if (press_seen_[button]) {
            release_pending_[button] = true;
        } else {
            down_[button] = false;
        }
        if (!(down_[0] || down_[1] || down_[2]) && ::GetCapture() == wnd) {
            ::ReleaseCapture();
        }
    };

    switch (msg) {
    case WM_MOUSEMOVE:
        mouse_ = {static_cast<f32>(GET_X_LPARAM(lparam)), static_cast<f32>(GET_Y_LPARAM(lparam))};
        if (!tracking_leave_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, wnd, 0};
            tracking_leave_ = ::TrackMouseEvent(&tme) != FALSE;
        }
        return true;
    case WM_MOUSELEAVE:
        tracking_leave_ = false;
        if (!(down_[0] || down_[1] || down_[2])) {
            mouse_ = {-1.0e6f, -1.0e6f};
        }
        return true;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: press(0); return true;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: press(1); return true;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: press(2); push_press(4); return true;
    case WM_LBUTTONUP: release(0); return true;
    case WM_RBUTTONUP: release(1); return true;
    case WM_MBUTTONUP: release(2); return true;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
        push_press(GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? 5u : 6u);
        return true;
    case WM_MOUSEWHEEL:
        wheel_ += static_cast<f32>(GET_WHEEL_DELTA_WPARAM(wparam)) / static_cast<f32>(WHEEL_DELTA);
        return true;
    case WM_MOUSEHWHEEL: // a tilt wheel or a trackpad swipe: positive is towards the right
        wheel_x_ += static_cast<f32>(GET_WHEEL_DELTA_WPARAM(wparam)) / static_cast<f32>(WHEEL_DELTA);
        return true;
    case WM_SETTINGCHANGE: // "ImmersiveColorSet": dark / light mode or the accent; SPI_SETHIGHCONTRAST: high contrast
        if ((lparam != 0 && std::wstring_view{reinterpret_cast<const wchar_t*>(lparam)} == L"ImmersiveColorSet") ||
            wparam == SPI_SETHIGHCONTRAST) {
            appearance_changed_ = true;
        }
        return false; // (the window procedure may want it too)
    case WM_SYSCOLORCHANGE:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        appearance_changed_ = true;
        return false;
    case WM_KILLFOCUS:
        down_            = {};
        release_pending_ = {};
        press_seen_      = {};
        return true;

    case WM_CHAR: {
        const auto unit = static_cast<char16_t>(wparam);
        if (unit >= 0xd800 && unit <= 0xdbff) {
            high_surrogate_ = unit;
            return true;
        }
        char32_t cp = unit;
        if (unit >= 0xdc00 && unit <= 0xdfff) {
            if (high_surrogate_ == 0) { return true; }
            cp = 0x10000u + ((static_cast<char32_t>(high_surrogate_) - 0xd800u) << 10) + (unit - 0xdc00u);
            high_surrogate_ = 0;
        }
        if (cp >= 32 && cp != 127) { // control characters arrive as key events instead
            push_text(cp);
        }
        return true;
    }

    case WM_IME_SETCONTEXT:
        // the ui draws the composition itself: keep the input method's own composition window away (its candidate list stays)
        ::DefWindowProcW(wnd, msg, wparam, wparam != 0 ? (lparam & ~static_cast<std::intptr_t>(ISC_SHOWUICOMPOSITIONWINDOW)) : lparam);
        return true;
    case WM_IME_STARTCOMPOSITION:
        ime_len_ = ime_cursor_ = 0;
        return true;
    case WM_IME_ENDCOMPOSITION:
        ime_len_ = ime_cursor_ = 0;
        return true;
    case WM_IME_CHAR:
        return true; // (the result comes through WM_IME_COMPOSITION)
    case WM_IME_COMPOSITION: {
        HIMC imc = ::ImmGetContext(wnd);
        if (imc == nullptr) {
            return true;
        }
        const auto read = [&](DWORD what) {
            std::wstring w;
            const LONG bytes = ::ImmGetCompositionStringW(imc, what, nullptr, 0);
            if (bytes > 0) {
                w.resize(static_cast<std::size_t>(bytes) / sizeof(wchar_t));
                ::ImmGetCompositionStringW(imc, what, w.data(), static_cast<DWORD>(bytes));
            }
            return w;
        };
        if ((lparam & GCS_RESULTSTR) != 0) { // confirmed text: typed, like any other
            const std::wstring w = read(GCS_RESULTSTR);
            for (std::size_t i = 0; i < w.size(); ++i) {
                char32_t cp = w[i];
                if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < w.size() && w[i + 1] >= 0xdc00 && w[i + 1] <= 0xdfff) {
                    cp = 0x10000u + ((cp - 0xd800u) << 10) + (static_cast<char32_t>(w[i + 1]) - 0xdc00u);
                    ++i;
                }
                if (cp >= 32 && cp != 127) { push_text(cp); }
            }
        }
        if ((lparam & GCS_COMPSTR) != 0) {
            const std::wstring w = read(GCS_COMPSTR);
            const auto utf8 = [](const wchar_t* p, std::size_t n, char* out, std::size_t cap) -> std::size_t {
                if (n == 0) { return 0; }
                const int len = ::WideCharToMultiByte(CP_UTF8, 0, p, static_cast<int>(n), out, static_cast<int>(cap), nullptr, nullptr);
                return len > 0 ? static_cast<std::size_t>(len) : 0;
            };
            ime_len_ = static_cast<u32>(utf8(w.data(), w.size(), ime_.data(), ime_.size()));
            // the caret: given in wide characters, wanted in utf-8 bytes
            const LONG cur = ::ImmGetCompositionStringW(imc, GCS_CURSORPOS, nullptr, 0);
            const std::size_t wide_cursor = cur > 0 ? std::min<std::size_t>(static_cast<std::size_t>(cur), w.size()) : 0;
            std::array<char, 256> tmp{};
            ime_cursor_ = std::min(static_cast<u32>(utf8(w.data(), wide_cursor, tmp.data(), tmp.size())), ime_len_);
            if (cur < 0) { ime_cursor_ = ime_len_; }
        } else {
            ime_len_ = ime_cursor_ = 0; // a result without composition text, or the composition was cancelled
        }
        ::ImmReleaseContext(wnd, imc);
        return true;
    }

    case WM_SYSCHAR: // Alt + letter: the ui reads it as a mnemonic (the key press itself is in pressed_key); no system beep
        if ((wparam >= 'a' && wparam <= 'z') || (wparam >= 'A' && wparam <= 'Z') || (wparam >= '0' && wparam <= '9')) {
            return true;
        }
        return false;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        // remember the key for hotkey binding unless it is a modifier
        if (wparam != VK_SHIFT && wparam != VK_CONTROL && wparam != VK_MENU && (wparam < VK_LSHIFT || wparam > VK_RMENU) &&
            wparam != VK_LWIN && wparam != VK_RWIN) {
            push_press(static_cast<u32>(wparam));
        }
        const bool ctrl  = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool alt   = (::GetKeyState(VK_MENU) & 0x8000) != 0;
        switch (wparam) {
        case VK_LEFT:   push_key(key::left, ctrl, shift, alt); break;
        case VK_RIGHT:  push_key(key::right, ctrl, shift, alt); break;
        case VK_UP:     push_key(key::up, ctrl, shift, alt); break;
        case VK_DOWN:   push_key(key::down, ctrl, shift, alt); break;
        case VK_HOME:   push_key(key::home, ctrl, shift, alt); break;
        case VK_END:    push_key(key::end, ctrl, shift, alt); break;
        case VK_BACK:   push_key(key::backspace, ctrl, shift, alt); break;
        case VK_DELETE: push_key(key::del, ctrl, shift, alt); break;
        case VK_RETURN: push_key(key::enter, ctrl, shift, alt); break;
        case VK_ESCAPE: push_key(key::escape, ctrl, shift, alt); break;
        case VK_TAB:    push_key(key::tab, ctrl, shift, alt); break;
        case VK_PRIOR:  push_key(key::page_up, ctrl, shift, alt); break;
        case VK_NEXT:   push_key(key::page_down, ctrl, shift, alt); break;
        case 'A': if (ctrl) { push_key(key::a, ctrl, shift, alt); } break;
        case 'C': if (ctrl) { push_key(key::c, ctrl, shift, alt); } break;
        case 'V': if (ctrl) { push_key(key::v, ctrl, shift, alt); } break;
        case 'X': if (ctrl) { push_key(key::x, ctrl, shift, alt); } break;
        case 'Z': if (ctrl) { push_key(key::z, ctrl, shift, alt); } break;
        case 'Y': if (ctrl) { push_key(key::y, ctrl, shift, alt); } break;
        default: break;
        }
        return true;
    }
    default:
        return false;
    }
}

bool win32_platform::swallows(std::uint32_t msg) noexcept
{
    return msg == WM_IME_SETCONTEXT || msg == WM_IME_STARTCOMPOSITION || msg == WM_IME_ENDCOMPOSITION || msg == WM_IME_COMPOSITION ||
           msg == WM_IME_CHAR;
}

void win32_platform::set_ime(bool wanted, vec2 caret_bottom_left, f32 caret_height) noexcept
{
    if (hwnd_ == nullptr) {
        return;
    }
    const auto wnd = static_cast<HWND>(hwnd_);
    if (wanted != ime_enabled_) {
        ime_enabled_ = wanted;
        if (wanted) {
            ::ImmAssociateContextEx(wnd, nullptr, IACE_DEFAULT);
        } else {
            ::ImmAssociateContext(wnd, nullptr);
            ime_len_ = ime_cursor_ = 0;
        }
        ime_pos_ = {-1.0f, -1.0f};
    }
    if (!wanted || (caret_bottom_left.x == ime_pos_.x && caret_bottom_left.y == ime_pos_.y)) {
        return;
    }
    ime_pos_ = caret_bottom_left;
    if (HIMC imc = ::ImmGetContext(wnd)) {
        COMPOSITIONFORM cf{};
        cf.dwStyle      = CFS_POINT;
        cf.ptCurrentPos = {static_cast<LONG>(caret_bottom_left.x), static_cast<LONG>(caret_bottom_left.y - caret_height)};
        ::ImmSetCompositionWindow(imc, &cf);
        CANDIDATEFORM cand{};
        cand.dwIndex        = 0;
        cand.dwStyle        = CFS_CANDIDATEPOS;
        cand.ptCurrentPos   = {static_cast<LONG>(caret_bottom_left.x), static_cast<LONG>(caret_bottom_left.y)};
        ::ImmSetCandidateWindow(imc, &cand);
        ::ImmReleaseContext(wnd, imc);
    }
}

input_state win32_platform::new_frame() noexcept
{
    input_state in;
    in.ime        = ime_;
    in.ime_len    = ime_len_;
    in.ime_cursor = ime_cursor_;
    in.mouse_pos  = mouse_;
    in.mouse_down = down_;
    in.wheel      = std::exchange(wheel_, 0.0f);
    in.wheel_x    = std::exchange(wheel_x_, 0.0f);

    in.keys      = keys_;
    in.key_count = std::exchange(key_count_, 0);
    in.typed     = typed_;
    in.typed_len = std::exchange(typed_len_, 0);
    detail::secure_wipe(typed_.data(), typed_.size()); // the copy in `in` is the only one left
    in.pressed_key = std::exchange(pressed_key_, 0);
    in.presses     = presses_;
    in.press_count = std::exchange(press_count_, 0);
    in.ctrl  = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
    in.shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
    in.alt   = (::GetKeyState(VK_MENU) & 0x8000) != 0;

    // every key held right now, so context::key_down() can answer about keys the ui has no name for. GetKeyboardState
    // is one call for all 256 of them and, unlike GetAsyncKeyState, reports the state this message queue has seen --
    // which is what the rest of the frame's input is in step with.
    std::array<BYTE, 256> vk{};
    if (::GetKeyboardState(vk.data())) {
        for (std::size_t i = 0; i < vk.size(); ++i) {
            in.set_held(static_cast<u32>(i), (vk[i] & 0x80) != 0);
        }
    }

    // the user's settings (Control Panel / accessibility): INFINITE is "do not blink"
    const UINT blink_ms = ::GetCaretBlinkTime();
    in.caret_blink_time  = blink_ms == INFINITE || blink_ms == 0 ? 0.0f : static_cast<f32>(blink_ms) / 1000.0f;
    in.double_click_time = static_cast<f32>(::GetDoubleClickTime()) / 1000.0f;

    const std::int64_t now = query_ticks();
    in.delta_time = static_cast<f32>(now - last_ticks_) / static_cast<f32>(tick_frequency());
    last_ticks_   = now;

    RECT rc{};
    if (hwnd_ != nullptr && ::GetClientRect(static_cast<HWND>(hwnd_), &rc)) {
        in.display_size = {static_cast<f32>(rc.right - rc.left), static_cast<f32>(rc.bottom - rc.top)};
    }

    // apply deferred releases now that this frame saw the press
    for (std::size_t i = 0; i < 3; ++i) {
        press_seen_[i] = false;
        if (release_pending_[i]) {
            down_[i]            = false;
            release_pending_[i] = false;
        }
    }
    return in;
}

} // namespace strata
