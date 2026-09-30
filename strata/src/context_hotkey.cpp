// key names and the hotkey / chord / sequence capture fields

#include "strata/context.hpp"

#include "context_impl.hpp"
#include "core/part_id.hpp"
#include "text_util.hpp"
#include "widget_util.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <numbers>
#include <optional>
#include <string>

namespace strata {

using namespace text;

std::string_view key_name(key k) noexcept
{
    static constexpr char letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static constexpr char digits[]  = "0123456789";
    static constexpr std::array<std::string_view, 24> fkeys = {
        "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
        "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24"};
    const auto vk = static_cast<u32>(k);

    if (vk >= 'A' && vk <= 'Z') { return {&letters[vk - 'A'], 1}; }
    if (vk >= '0' && vk <= '9') { return {&digits[vk - '0'], 1}; }
    if (vk >= 0x70 && vk <= 0x87) { return fkeys[vk - 0x70]; }
    if (vk >= 0x60 && vk <= 0x69) {
        static constexpr std::array<std::string_view, 10> pad = {"Num 0", "Num 1", "Num 2", "Num 3", "Num 4",
                                                                  "Num 5", "Num 6", "Num 7", "Num 8", "Num 9"};
        return pad[vk - 0x60];
    }
    switch (vk) {
    case 0:    return "None";
    case 0x01: return "Mouse 1";
    case 0x02: return "Mouse 2";
    case 0x04: return "Mouse 3";
    case 0x05: return "Mouse 4";
    case 0x06: return "Mouse 5";
    case 0x08: return "Backspace";
    case 0x09: return "Tab";
    case 0x0d: return "Enter";
    case 0x13: return "Pause";
    case 0x14: return "Caps Lock";
    case 0x1b: return "Esc";
    case 0x20: return "Space";
    case 0x21: return "Page Up";
    case 0x22: return "Page Down";
    case 0x23: return "End";
    case 0x24: return "Home";
    case 0x25: return "Left";
    case 0x26: return "Up";
    case 0x27: return "Right";
    case 0x28: return "Down";
    case 0x2c: return "Print Screen";
    case 0x2d: return "Insert";
    case 0x2e: return "Delete";
    case 0x6a: return "Num *";
    case 0x6b: return "Num +";
    case 0x6d: return "Num -";
    case 0x6e: return "Num .";
    case 0x6f: return "Num /";
    case 0x90: return "Num Lock";
    case 0x91: return "Scroll Lock";
    case 0xba: return ";";
    case 0xbb: return "=";
    case 0xbc: return ",";
    case 0xbd: return "-";
    case 0xbe: return ".";
    case 0xbf: return "/";
    case 0xc0: return "`";
    case 0xdb: return "[";
    case 0xdc: return "\\";
    case 0xdd: return "]";
    case 0xde: return "'";
    default:   return "Key ?";
    }
}

bool context::hotkey(std::string_view label, strata::key& key_code)
{
    return hotkey_field(label, key_code, nullptr);
}

bool context::hotkey_chord(std::string_view label, key_chord& chord)
{
    return hotkey_field(label, chord.key, &chord);
}

bool context::hotkey_sequence(std::string_view label, key_sequence& seq)
{
    if (m_->cur_ == nullptr) {
        return false;
    }
    const font_id f = current_font();
    const id key = widget_id(label);

    const field_layout fl  = layout_field(visible_label(label), frame_height());
    const rect         box = fl.control;
    const interaction  in  = interact(key, box);

    bool capturing = m_->hotkey_.hotkey_capture_ == key;
    if (in.pressed) {
        capturing       = !capturing;
        m_->hotkey_.hotkey_capture_ = capturing ? key : 0;
        m_->hotkey_.seq_edit_count_ = 0;
    } else if (capturing && m_->input_.mouse_pressed_ && !box.contains(m_->input_.mouse_)) {
        capturing       = false;
        m_->hotkey_.hotkey_capture_ = 0;
    }

    // each step commits at once (no latency for single chords), but capture continues briefly so another key extends
    // it into a sequence, replacing the committed one
    bool changed = false;
    if (capturing && m_->input_.pressed_key_ != strata::key::none) {
        const bool bare = !(m_->input_.press_ctrl_ || m_->input_.press_shift_ || m_->input_.press_alt_);
        if (m_->hotkey_.seq_edit_count_ == 0 && m_->input_.pressed_key_ == strata::key::escape && bare) {                                   // Esc: leave it as it was
            capturing       = false;
            m_->hotkey_.hotkey_capture_ = 0;
        } else if (m_->hotkey_.seq_edit_count_ == 0 && (m_->input_.pressed_key_ == strata::key::backspace || m_->input_.pressed_key_ == strata::key::del) && bare) {  // Backspace / Delete: unbind
            changed         = seq.bound();
            seq             = {};
            capturing       = false;
            m_->hotkey_.hotkey_capture_ = 0;
        } else {
            m_->hotkey_.seq_edit_capture_[m_->hotkey_.seq_edit_count_++] = {m_->input_.pressed_key_, m_->input_.press_ctrl_, m_->input_.press_shift_, m_->input_.press_alt_};
            m_->hotkey_.seq_edit_deadline_                   = m_->time_ + key_sequence_timeout;
            key_sequence next;
            for (u8 i = 0; i < m_->hotkey_.seq_edit_count_; ++i) { next.steps[next.count++] = m_->hotkey_.seq_edit_capture_[i]; }
            changed = next != seq;
            seq     = next;
            if (m_->hotkey_.seq_edit_count_ >= key_sequence::max_steps) { // no room for another step: definitely done
                capturing       = false;
                m_->hotkey_.hotkey_capture_ = 0;
            }
        }
        m_->input_.pressed_key_ = strata::key::none; // the key is spent either way; do not also fire an accelerator
        m_->input_.key_count_   = 0;
    } else if (capturing && m_->hotkey_.seq_edit_count_ > 0 && m_->time_ >= m_->hotkey_.seq_edit_deadline_) {
        // paused without a further key: what was captured already stands, just stop listening for more
        capturing       = false;
        m_->hotkey_.hotkey_capture_ = 0;
    }
    if (capturing) {
        m_->hotkey_.hotkey_seen_ = true;
    }

    anim_slot& a = anim_for(key);
    a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(a.toggle, capturing ? 1.0f : 0.0f);

    shape_style field = widget_shape(lerp(m_->style_.widget_bg, m_->style_.widget_hover, a.hover), m_->style_.rounding * 0.8f);
    field.border = lerp(lerp(m_->style_.widget_border, m_->style_.accent_hover, a.hover * 0.45f), m_->style_.accent, a.toggle);
    field.shadow      = m_->style_.accent.scaled_alpha(0.3f * a.toggle);
    field.shadow_blur = 8.0f * a.toggle;
    m_->dl_.shape(box, field);

    std::string shown_text;
    if (capturing && m_->hotkey_.seq_edit_count_ > 0) {
        shown_text = sequence_to_string(seq) + ", ..."; // committed so far; a further key would extend it
    } else if (!capturing) {
        shown_text = sequence_to_string(seq);
    }
    const std::string_view shown = capturing && m_->hotkey_.seq_edit_count_ == 0 ? m_->strings_.press_a_key
                                  : !capturing && shown_text.empty() ? m_->strings_.unbound
                                                                     : std::string_view{shown_text};
    color tc = !capturing && !seq.bound() ? m_->style_.text_dim : m_->style_.text;
    if (capturing) {
        const f32 pulse = 0.65f + 0.35f * std::sin(static_cast<f32>(m_->time_) * 7.0f);
        tc = m_->style_.accent_hover.scaled_alpha(pulse);
    }
    const vec2 tsize = m_->font_.measure(f, shown);
    m_->dl_.text({box.min.x + (box.width() - tsize.x) * 0.5f, box.min.y + (box.height() - tsize.y) * 0.5f}, tc, shown, f);
    track_edit(key, changed, m_->hotkey_.hotkey_capture_ == key);
    return changed;
}

// `chord` null = plain key; otherwise key_code is chord->key and the held modifiers are stored too
bool context::hotkey_field(std::string_view label, strata::key& key_code, key_chord* chord)
{
    if (m_->cur_ == nullptr) {
        return false;
    }
    const font_id f = current_font();
    const id key = widget_id(label);

    const field_layout fl  = layout_field(visible_label(label), frame_height());
    const rect         box = fl.control;
    const interaction  in  = interact(key, box);

    bool capturing = m_->hotkey_.hotkey_capture_ == key;
    if (in.pressed) {
        capturing       = !capturing;
        m_->hotkey_.hotkey_capture_ = capturing ? key : 0;
    } else if (capturing && m_->input_.mouse_pressed_ && !box.contains(m_->input_.mouse_)) {
        capturing       = false;
        m_->hotkey_.hotkey_capture_ = 0;
    }

    bool changed = false;
    if (capturing && m_->input_.pressed_key_ != strata::key::none) {
        const bool bare = chord == nullptr || !(m_->input_.press_ctrl_ || m_->input_.press_shift_ || m_->input_.press_alt_);
        if (m_->input_.pressed_key_ == strata::key::escape && bare) {                                     // Esc: leave it as it was
        } else if ((m_->input_.pressed_key_ == strata::key::backspace || m_->input_.pressed_key_ == strata::key::del) && bare) {   // Backspace / Delete: unbind
            changed  = key_code != strata::key::none;
            key_code = strata::key::none;
            if (chord != nullptr) { *chord = {}; }
        } else {
            const key_chord next{m_->input_.pressed_key_, m_->input_.press_ctrl_, m_->input_.press_shift_, m_->input_.press_alt_};
            changed  = chord != nullptr ? *chord != next : key_code != m_->input_.pressed_key_;
            key_code = m_->input_.pressed_key_;
            if (chord != nullptr) { *chord = next; }
        }
        capturing       = false;
        m_->hotkey_.hotkey_capture_ = 0;
        m_->input_.pressed_key_ = strata::key::none;
        m_->input_.key_count_      = 0;
    }
    if (capturing) {
        m_->hotkey_.hotkey_seen_ = true;
    }

    anim_slot& a = anim_for(key);
    a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(a.toggle, capturing ? 1.0f : 0.0f);

    shape_style field = widget_shape(lerp(m_->style_.widget_bg, m_->style_.widget_hover, a.hover), m_->style_.rounding * 0.8f);
    field.border = lerp(lerp(m_->style_.widget_border, m_->style_.accent_hover, a.hover * 0.45f), m_->style_.accent, a.toggle);
    field.shadow      = m_->style_.accent.scaled_alpha(0.3f * a.toggle);
    field.shadow_blur = 8.0f * a.toggle;
    m_->dl_.shape(box, field);

    std::string chord_text;
    if (chord != nullptr && !capturing) { chord_text = chord_to_string(*chord); }
    std::string_view shown = capturing ? m_->strings_.press_a_key
                           : chord != nullptr ? (chord_text.empty() ? m_->strings_.unbound : std::string_view{chord_text})
                                              : key_code == strata::key::none ? m_->strings_.unbound : key_name(key_code);
    color tc = key_code == strata::key::none && !capturing ? m_->style_.text_dim : m_->style_.text;
    if (capturing) {
        const f32 pulse = 0.65f + 0.35f * std::sin(static_cast<f32>(m_->time_) * 7.0f);
        tc = m_->style_.accent_hover.scaled_alpha(pulse);
    }
    const vec2 tsize = m_->font_.measure(f, shown);
    m_->dl_.text({box.min.x + (box.width() - tsize.x) * 0.5f, box.min.y + (box.height() - tsize.y) * 0.5f}, tc, shown, f);
    track_edit(key, changed, m_->hotkey_.hotkey_capture_ == key);
    return changed;
}

} // namespace strata
