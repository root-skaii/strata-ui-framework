#include "strata/input_record.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace strata {

namespace {

constexpr char magic[8]       = {'S', 'T', 'R', 'A', 'T', 'A', 'I', 'N'};
constexpr u32  format_version = 1;

class writer {
public:
    explicit writer(std::vector<u8>& out) : out_{out} {}
    template <class T>
    void put(const T& v) { put_bytes(&v, sizeof(T)); }
    void put_bytes(const void* p, std::size_t n)
    {
        const auto* b = static_cast<const u8*>(p);
        out_.insert(out_.end(), b, b + n);
    }

private:
    std::vector<u8>& out_;
};

class reader {
public:
    explicit reader(std::span<const u8> in) : in_{in} {}
    template <class T>
    bool get(T& v) noexcept { return get_bytes(&v, sizeof(T)); }
    bool get_bytes(void* p, std::size_t n) noexcept
    {
        if (in_.size() - pos_ < n) { return false; }
        std::memcpy(p, in_.data() + pos_, n);
        pos_ += n;
        return true;
    }
    [[nodiscard]] std::size_t position() const noexcept { return pos_; }
    [[nodiscard]] bool        done() const noexcept { return pos_ == in_.size(); }
    void                      skip(std::size_t n) noexcept { pos_ += n; }

private:
    std::span<const u8> in_;
    std::size_t         pos_{};
};

void encode(writer& w, const input_state& in)
{
    w.put(in.mouse_pos);
    w.put(in.mouse_down);
    w.put(in.wheel);
    w.put(in.wheel_x);
    w.put(in.delta_time);
    w.put(in.display_size);
    w.put(in.caret_blink_time);
    w.put(in.double_click_time);
    w.put(in.wheel_lines);
    w.put(static_cast<u8>(in.ctrl));
    w.put(static_cast<u8>(in.shift));
    w.put(static_cast<u8>(in.alt));
    w.put(in.keys_held);
    w.put(in.key_count);
    for (u32 i = 0; i < in.key_count; ++i) {
        const key_event& k = in.keys[i];
        w.put(static_cast<u32>(k.k));
        w.put(static_cast<u8>((k.ctrl ? 1 : 0) | (k.shift ? 2 : 0) | (k.alt ? 4 : 0)));
    }
    w.put(in.typed_len);
    w.put_bytes(in.typed.data(), in.typed_len);
    w.put(in.ime_len);
    w.put_bytes(in.ime.data(), in.ime_len);
    w.put(in.ime_cursor);
}

[[nodiscard]] bool decode(reader& r, input_state& in) noexcept
{
    in = input_state{};
    u8 ctrl{}, shift{}, alt{};
    if (!r.get(in.mouse_pos) || !r.get(in.mouse_down) || !r.get(in.wheel) || !r.get(in.wheel_x) || !r.get(in.delta_time) ||
        !r.get(in.display_size) || !r.get(in.caret_blink_time) || !r.get(in.double_click_time) || !r.get(in.wheel_lines) ||
        !r.get(ctrl) || !r.get(shift) || !r.get(alt) || !r.get(in.keys_held) || !r.get(in.key_count)) {
        return false;
    }
    in.ctrl  = ctrl != 0;
    in.shift = shift != 0;
    in.alt   = alt != 0;
    if (in.key_count > in.keys.size()) { return false; }
    for (u32 i = 0; i < in.key_count; ++i) {
        u32 k{};
        u8  mods{};
        if (!r.get(k) || !r.get(mods)) { return false; }
        in.keys[i] = {static_cast<key>(k), (mods & 1) != 0, (mods & 2) != 0, (mods & 4) != 0};
    }
    if (!r.get(in.typed_len) || in.typed_len > in.typed.size() || !r.get_bytes(in.typed.data(), in.typed_len)) { return false; }
    if (!r.get(in.ime_len) || in.ime_len > in.ime.size() || !r.get_bytes(in.ime.data(), in.ime_len)) { return false; }
    return r.get(in.ime_cursor);
}

[[nodiscard]] std::filesystem::path to_path(std::string_view utf8)
{
    return std::filesystem::path{std::u8string{utf8.begin(), utf8.end()}};
}

} // namespace

input_recorder::input_recorder() { clear(); }

void input_recorder::clear()
{
    bytes_.clear();
    frames_ = 0;
    writer w{bytes_};
    w.put_bytes(magic, sizeof magic);
    w.put(format_version);
}

void input_recorder::record(const input_state& in)
{
    std::vector<u8> payload;
    writer pw{payload};
    encode(pw, in);
    writer w{bytes_};
    w.put(static_cast<u32>(payload.size()));
    w.put_bytes(payload.data(), payload.size());
    ++frames_;
}

bool input_recorder::save(std::string_view path) const
{
    std::ofstream f{to_path(path), std::ios::binary | std::ios::trunc};
    f.write(reinterpret_cast<const char*>(bytes_.data()), static_cast<std::streamsize>(bytes_.size()));
    return f.good();
}

bool input_replayer::load(std::span<const u8> bytes)
{
    reader r{bytes};
    char   head[sizeof magic];
    u32    version{};
    if (!r.get_bytes(head, sizeof head) || std::memcmp(head, magic, sizeof magic) != 0 || !r.get(version) ||
        version != format_version) {
        return false;
    }
    std::vector<std::size_t> offsets;
    input_state              scratch;
    while (!r.done()) {
        u32 size{};
        if (!r.get(size)) { return false; }
        const std::size_t start = r.position();
        if (size > bytes.size() - start) { return false; }
        reader frame{bytes.subspan(start, size)};
        if (!decode(frame, scratch) || !frame.done()) { return false; }
        offsets.push_back(start);
        r.skip(size);
    }
    bytes_.assign(bytes.begin(), bytes.end());
    offsets_  = std::move(offsets);
    position_ = 0;
    last_     = input_state{};
    return true;
}

bool input_replayer::load_file(std::string_view path)
{
    std::ifstream f{to_path(path), std::ios::binary};
    if (!f) { return false; }
    const std::vector<char> raw{std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{}};
    return load({reinterpret_cast<const u8*>(raw.data()), raw.size()});
}

bool input_replayer::next(input_state& out) noexcept
{
    if (position_ < offsets_.size()) {
        const std::size_t start = offsets_[position_++];
        u32 size{};
        std::memcpy(&size, bytes_.data() + start - sizeof size, sizeof size);
        reader r{std::span<const u8>{bytes_}.subspan(start, size)};
        (void)decode(r, last_); // validated by load()
        out = last_;
        return true;
    }
    last_.key_count = 0;
    last_.typed_len = 0;
    last_.ime_len   = 0;
    last_.wheel     = 0.0f;
    last_.wheel_x   = 0.0f;
    out             = last_;
    return false;
}

} // namespace strata
