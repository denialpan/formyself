#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <commdlg.h>
#include <bcrypt.h>
#include <windows.h>

#include "argon2.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

enum class PendingAction {
    None,
    NewFile,
    OpenFile,
    CloseApp,
};

enum class EditorCommand {
    None,
    Undo,
    Redo,
    Cut,
    Copy,
    Paste,
    InsertText,
    SelectAll,
    FindNext,
    FindPrevious,
};

enum class PasswordPromptMode {
    None,
    Save,
    Open,
};

struct AppConfig {
    int window_width = 1000;
    int window_height = 700;
    bool word_wrap = true;
    bool show_line_numbers = true;
    int generated_password_length = 24;
    bool generated_use_uppercase = true;
    bool generated_use_lowercase = true;
    bool generated_use_numbers = true;
    bool generated_use_special = true;
};

struct EditorSnapshot {
    std::string text;
    int cursor_pos = 0;
    int selection_start = 0;
    int selection_end = 0;
    bool has_selection = false;
};

struct Document {
    std::string text;
    std::string saved_text;
    std::vector<std::string> lines{""};
    std::vector<size_t> line_offsets{0};
    size_t saved_revision = 0;
    size_t edit_revision = 0;

    void RebuildLineIndex() {
        line_offsets.clear();
        line_offsets.push_back(0);
        lines.clear();

        size_t line_start = 0;

        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\n') {
                line_offsets.push_back(i + 1);
                lines.push_back(text.substr(line_start, i - line_start));
                line_start = i + 1;
            }
        }

        lines.push_back(text.substr(line_start));
    }

    void SetText(std::string contents) {
        text = std::move(contents);
        text.reserve(text.size() + 1);
        RebuildLineIndex();
        ++edit_revision;
    }

    void Clear() {
        text.clear();
        text.reserve(1);
        RebuildLineIndex();
        ++edit_revision;
        MarkSaved();
    }

    void MarkEdited() {
        ++edit_revision;
        RebuildLineIndex();
    }

    void MarkSaved() {
        saved_text = text;
        saved_revision = edit_revision;
    }

    bool IsDirty() const {
        return text != saved_text;
    }

    int LineCount() const {
        return static_cast<int>(lines.empty() ? 1 : lines.size());
    }

    size_t LineStart(int one_based_line) const {
        const int line = std::clamp(one_based_line, 1, LineCount());
        return line_offsets[static_cast<size_t>(line - 1)];
    }

    size_t LineStartByIndex(size_t line_index) const {
        if (line_offsets.empty()) {
            return 0;
        }
        return line_offsets[(std::min)(line_index, line_offsets.size() - 1)];
    }

    size_t PositionFromLineColumn(size_t line_index, size_t column) const {
        if (lines.empty()) {
            return 0;
        }

        const size_t clamped_line = (std::min)(line_index, lines.size() - 1);
        const size_t clamped_column = (std::min)(column, lines[clamped_line].size());
        return LineStartByIndex(clamped_line) + clamped_column;
    }

    void CursorLineColumn(size_t cursor_pos, int& line, int& column) const {
        const size_t clamped = (std::min)(cursor_pos, text.size());
        const auto it = std::upper_bound(line_offsets.begin(), line_offsets.end(), clamped);
        const size_t line_index = it == line_offsets.begin()
            ? 0
            : static_cast<size_t>(std::distance(line_offsets.begin(), it) - 1);

        line = static_cast<int>(line_index) + 1;
        column = static_cast<int>(clamped - line_offsets[line_index]) + 1;
    }

    std::pair<size_t, size_t> LineColumnFromPosition(size_t cursor_pos) const {
        int line = 1;
        int column = 1;
        CursorLineColumn(cursor_pos, line, column);
        return {
            static_cast<size_t>((std::max)(0, line - 1)),
            static_cast<size_t>((std::max)(0, column - 1)),
        };
    }

    void Insert(size_t position, std::string_view inserted_text) {
        text.insert((std::min)(position, text.size()), inserted_text.data(), inserted_text.size());
        MarkEdited();
    }

    void Erase(size_t position, size_t length) {
        if (position >= text.size() || length == 0) {
            return;
        }

        text.erase(position, (std::min)(length, text.size() - position));
        MarkEdited();
    }
};

struct EditorState {
    Document document;
    std::filesystem::path file_path;
    std::string status = "Ready";
    std::string last_action = "New document";
    bool word_wrap = true;
    bool show_line_numbers = true;
    bool encrypted_document = false;
    float wrap_width = 0.0f;
    float editor_scroll_y = 0.0f;
    std::vector<float> visual_line_offsets{0.0f};
    std::vector<float> visual_line_heights;
    size_t visual_metrics_revision = (std::numeric_limits<size_t>::max)();
    float visual_metrics_wrap_width = -1.0f;
    bool visual_metrics_word_wrap = false;
    std::vector<EditorSnapshot> undo_stack;
    std::vector<EditorSnapshot> redo_stack;
    int selection_start = 0;
    int selection_end = 0;
    int cursor_pos = 0;
    bool has_selection = false;
    bool editor_focused = false;
    size_t desired_column = 0;
    size_t mouse_selection_anchor = 0;
    bool dragging_selection = false;
    int cursor_line = 1;
    int cursor_column = 1;
    PendingAction pending_action = PendingAction::None;
    EditorCommand pending_editor_command = EditorCommand::None;
    std::string pending_insert_text;
    PasswordPromptMode password_prompt_mode = PasswordPromptMode::None;
    std::filesystem::path password_target_path;
    std::array<char, 256> password_buffer{};
    std::array<char, 256> confirm_password_buffer{};
    std::string password_error;
    bool show_password_prompt = false;
    bool complete_pending_after_save = false;
    bool show_password_generator = false;
    int generated_password_length = 24;
    bool generated_use_uppercase = true;
    bool generated_use_lowercase = true;
    bool generated_use_numbers = true;
    bool generated_use_special = true;
    bool show_find_window = false;
    std::array<char, 256> find_buffer{};
    bool find_match_case = false;
    bool find_wrap = true;
    size_t active_find_start = std::string::npos;
    size_t active_find_end = std::string::npos;
    bool show_unsaved_prompt = false;
    bool close_after_frame = false;
};

struct VisualRow {
    int line = 0;
    size_t start = 0;
    size_t end = 0;
    float y = 0.0f;
};

struct InputCallbackState {
    Document* document = nullptr;
    EditorState* editor = nullptr;
};

int TextInputCallback(ImGuiInputTextCallbackData* data) {
    auto* state = static_cast<InputCallbackState*>(data->UserData);

    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        Document* document = state->document;
        document->text.resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = document->text.data();
        return 0;
    }

    if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways && state->editor != nullptr) {
        state->editor->wrap_width = ImGui::GetContentRegionAvail().x;
    }

    return 0;
}

std::string ReadFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("could not open file");
    }

    file.seekg(0, std::ios::end);
    const std::streamoff file_size = file.tellg();
    if (file_size < 0) {
        throw std::runtime_error("could not measure file size");
    }

    std::string contents;
    contents.resize(static_cast<size_t>(file_size));
    file.seekg(0, std::ios::beg);
    if (!contents.empty()) {
        file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!file) {
            throw std::runtime_error("could not read complete file");
        }
    }
    contents.reserve(contents.size() + 1);
    return contents;
}

void WriteFile(const std::string& path, const std::string& contents) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw std::runtime_error("could not write file");
    }

    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

std::filesystem::path ConfigPath() {
    return "editor_config.ini";
}

bool ParseBool(const std::string& value, bool fallback) {
    if (value == "1" || value == "true" || value == "True") {
        return true;
    }
    if (value == "0" || value == "false" || value == "False") {
        return false;
    }
    return fallback;
}

int ParseInt(const std::string& value, int fallback, int minimum, int maximum) {
    try {
        return std::clamp(std::stoi(value), minimum, maximum);
    } catch (...) {
        return fallback;
    }
}

AppConfig LoadConfig() {
    AppConfig config;
    std::ifstream file(ConfigPath());
    if (!file) {
        return config;
    }

    std::string line;
    while (std::getline(file, line)) {
        const size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 1);
        if (key == "window_width") {
            config.window_width = ParseInt(value, config.window_width, 400, 3840);
        } else if (key == "window_height") {
            config.window_height = ParseInt(value, config.window_height, 300, 2160);
        } else if (key == "word_wrap") {
            config.word_wrap = ParseBool(value, config.word_wrap);
        } else if (key == "show_line_numbers") {
            config.show_line_numbers = ParseBool(value, config.show_line_numbers);
        } else if (key == "generated_password_length") {
            config.generated_password_length = ParseInt(value, config.generated_password_length, 1, 256);
        } else if (key == "generated_use_uppercase") {
            config.generated_use_uppercase = ParseBool(value, config.generated_use_uppercase);
        } else if (key == "generated_use_lowercase") {
            config.generated_use_lowercase = ParseBool(value, config.generated_use_lowercase);
        } else if (key == "generated_use_numbers") {
            config.generated_use_numbers = ParseBool(value, config.generated_use_numbers);
        } else if (key == "generated_use_special") {
            config.generated_use_special = ParseBool(value, config.generated_use_special);
        }
    }

    return config;
}

void ApplyConfig(EditorState& editor, const AppConfig& config) {
    editor.word_wrap = config.word_wrap;
    editor.show_line_numbers = config.show_line_numbers;
    editor.generated_password_length = config.generated_password_length;
    editor.generated_use_uppercase = config.generated_use_uppercase;
    editor.generated_use_lowercase = config.generated_use_lowercase;
    editor.generated_use_numbers = config.generated_use_numbers;
    editor.generated_use_special = config.generated_use_special;
}

void SaveConfig(const EditorState& editor, GLFWwindow* window) {
    int window_width = 0;
    int window_height = 0;
    glfwGetWindowSize(window, &window_width, &window_height);

    std::ostringstream output;
    output << "window_width=" << std::clamp(window_width, 400, 3840) << '\n';
    output << "window_height=" << std::clamp(window_height, 300, 2160) << '\n';
    output << "word_wrap=" << (editor.word_wrap ? 1 : 0) << '\n';
    output << "show_line_numbers=" << (editor.show_line_numbers ? 1 : 0) << '\n';
    output << "generated_password_length=" << std::clamp(editor.generated_password_length, 1, 256) << '\n';
    output << "generated_use_uppercase=" << (editor.generated_use_uppercase ? 1 : 0) << '\n';
    output << "generated_use_lowercase=" << (editor.generated_use_lowercase ? 1 : 0) << '\n';
    output << "generated_use_numbers=" << (editor.generated_use_numbers ? 1 : 0) << '\n';
    output << "generated_use_special=" << (editor.generated_use_special ? 1 : 0) << '\n';

    WriteFile(ConfigPath().string(), output.str());
}

constexpr std::array<unsigned char, 8> encrypted_magic{'I', 'T', 'E', 'D', 'A', 'E', 'S', '1'};
constexpr uint32_t argon2_memory_kib = 64 * 1024;
constexpr uint32_t argon2_iterations = 3;
constexpr uint32_t argon2_parallelism = 1;
constexpr size_t aes_key_size = 32;
constexpr size_t aes_salt_size = 16;
constexpr size_t aes_nonce_size = 12;
constexpr size_t aes_tag_size = 16;

void AppendU32(std::string& output, uint32_t value) {
    output.push_back(static_cast<char>(value & 0xff));
    output.push_back(static_cast<char>((value >> 8) & 0xff));
    output.push_back(static_cast<char>((value >> 16) & 0xff));
    output.push_back(static_cast<char>((value >> 24) & 0xff));
}

uint32_t ReadU32(const std::string& input, size_t& offset) {
    if (offset + 4 > input.size()) {
        throw std::runtime_error("encrypted file header is truncated");
    }

    const auto* bytes = reinterpret_cast<const unsigned char*>(input.data() + offset);
    offset += 4;
    return static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
}

bool IsEncryptedFileContents(const std::string& contents) {
    return contents.size() >= encrypted_magic.size() &&
        std::equal(encrypted_magic.begin(), encrypted_magic.end(), contents.begin());
}

void ThrowIfFailed(NTSTATUS status, const char* operation) {
    if (status < 0) {
        throw std::runtime_error(std::string(operation) + " failed");
    }
}

void FillRandom(unsigned char* data, size_t size) {
    ThrowIfFailed(
        BCryptGenRandom(nullptr, data, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG),
        "random generation");
}

std::array<unsigned char, aes_key_size> DeriveKey(
    const std::string& password,
    const std::array<unsigned char, aes_salt_size>& salt,
    uint32_t memory_kib,
    uint32_t iterations,
    uint32_t parallelism) {
    std::array<unsigned char, aes_key_size> key{};
    const int result = argon2id_hash_raw(
        iterations,
        memory_kib,
        parallelism,
        password.data(),
        password.size(),
        salt.data(),
        salt.size(),
        key.data(),
        key.size());

    if (result != ARGON2_OK) {
        throw std::runtime_error(argon2_error_message(result));
    }

    return key;
}

struct BCryptAlgHandle {
    BCRYPT_ALG_HANDLE handle = nullptr;
    ~BCryptAlgHandle() {
        if (handle != nullptr) {
            BCryptCloseAlgorithmProvider(handle, 0);
        }
    }
};

struct BCryptKeyHandle {
    BCRYPT_KEY_HANDLE handle = nullptr;
    ~BCryptKeyHandle() {
        if (handle != nullptr) {
            BCryptDestroyKey(handle);
        }
    }
};

std::string AesGcmEncrypt(
    const std::string& plaintext,
    const std::array<unsigned char, aes_key_size>& key,
    const std::array<unsigned char, aes_nonce_size>& nonce,
    std::array<unsigned char, aes_tag_size>& tag) {
    BCryptAlgHandle algorithm;
    ThrowIfFailed(
        BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_AES_ALGORITHM, nullptr, 0),
        "AES provider open");
    ThrowIfFailed(
        BCryptSetProperty(
            algorithm.handle,
            BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
            static_cast<ULONG>((wcslen(BCRYPT_CHAIN_MODE_GCM) + 1) * sizeof(wchar_t)),
            0),
        "AES-GCM mode setup");

    BCryptKeyHandle key_handle;
    ThrowIfFailed(
        BCryptGenerateSymmetricKey(
            algorithm.handle,
            &key_handle.handle,
            nullptr,
            0,
            const_cast<PUCHAR>(key.data()),
            static_cast<ULONG>(key.size()),
            0),
        "AES key setup");

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth_info;
    BCRYPT_INIT_AUTH_MODE_INFO(auth_info);
    auth_info.pbNonce = const_cast<PUCHAR>(nonce.data());
    auth_info.cbNonce = static_cast<ULONG>(nonce.size());
    auth_info.pbTag = tag.data();
    auth_info.cbTag = static_cast<ULONG>(tag.size());

    std::string ciphertext(plaintext.size(), '\0');
    ULONG written = 0;
    ThrowIfFailed(
        BCryptEncrypt(
            key_handle.handle,
            reinterpret_cast<PUCHAR>(const_cast<char*>(plaintext.data())),
            static_cast<ULONG>(plaintext.size()),
            &auth_info,
            nullptr,
            0,
            reinterpret_cast<PUCHAR>(ciphertext.data()),
            static_cast<ULONG>(ciphertext.size()),
            &written,
            0),
        "AES encryption");
    ciphertext.resize(written);
    return ciphertext;
}

std::string AesGcmDecrypt(
    const std::string& ciphertext,
    const std::array<unsigned char, aes_key_size>& key,
    const std::array<unsigned char, aes_nonce_size>& nonce,
    const std::array<unsigned char, aes_tag_size>& tag) {
    BCryptAlgHandle algorithm;
    ThrowIfFailed(
        BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_AES_ALGORITHM, nullptr, 0),
        "AES provider open");
    ThrowIfFailed(
        BCryptSetProperty(
            algorithm.handle,
            BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
            static_cast<ULONG>((wcslen(BCRYPT_CHAIN_MODE_GCM) + 1) * sizeof(wchar_t)),
            0),
        "AES-GCM mode setup");

    BCryptKeyHandle key_handle;
    ThrowIfFailed(
        BCryptGenerateSymmetricKey(
            algorithm.handle,
            &key_handle.handle,
            nullptr,
            0,
            const_cast<PUCHAR>(key.data()),
            static_cast<ULONG>(key.size()),
            0),
        "AES key setup");

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth_info;
    BCRYPT_INIT_AUTH_MODE_INFO(auth_info);
    auth_info.pbNonce = const_cast<PUCHAR>(nonce.data());
    auth_info.cbNonce = static_cast<ULONG>(nonce.size());
    auth_info.pbTag = const_cast<PUCHAR>(tag.data());
    auth_info.cbTag = static_cast<ULONG>(tag.size());

    std::string plaintext(ciphertext.size(), '\0');
    ULONG written = 0;
    ThrowIfFailed(
        BCryptDecrypt(
            key_handle.handle,
            reinterpret_cast<PUCHAR>(const_cast<char*>(ciphertext.data())),
            static_cast<ULONG>(ciphertext.size()),
            &auth_info,
            nullptr,
            0,
            reinterpret_cast<PUCHAR>(plaintext.data()),
            static_cast<ULONG>(plaintext.size()),
            &written,
            0),
        "AES decryption or password verification");
    plaintext.resize(written);
    return plaintext;
}

std::string EncryptDocumentContents(const std::string& plaintext, const std::string& password) {
    std::array<unsigned char, aes_salt_size> salt{};
    std::array<unsigned char, aes_nonce_size> nonce{};
    std::array<unsigned char, aes_tag_size> tag{};
    FillRandom(salt.data(), salt.size());
    FillRandom(nonce.data(), nonce.size());

    const auto key = DeriveKey(password, salt, argon2_memory_kib, argon2_iterations, argon2_parallelism);
    const std::string ciphertext = AesGcmEncrypt(plaintext, key, nonce, tag);

    std::string output;
    output.reserve(encrypted_magic.size() + 8 * 4 + salt.size() + nonce.size() + tag.size() + ciphertext.size());
    output.append(reinterpret_cast<const char*>(encrypted_magic.data()), encrypted_magic.size());
    AppendU32(output, 1);
    AppendU32(output, argon2_memory_kib);
    AppendU32(output, argon2_iterations);
    AppendU32(output, argon2_parallelism);
    AppendU32(output, static_cast<uint32_t>(salt.size()));
    AppendU32(output, static_cast<uint32_t>(nonce.size()));
    AppendU32(output, static_cast<uint32_t>(tag.size()));
    AppendU32(output, static_cast<uint32_t>(ciphertext.size()));
    output.append(reinterpret_cast<const char*>(salt.data()), salt.size());
    output.append(reinterpret_cast<const char*>(nonce.data()), nonce.size());
    output.append(reinterpret_cast<const char*>(tag.data()), tag.size());
    output.append(ciphertext);
    return output;
}

std::string DecryptDocumentContents(const std::string& encrypted, const std::string& password) {
    if (!IsEncryptedFileContents(encrypted)) {
        return encrypted;
    }

    size_t offset = encrypted_magic.size();
    const uint32_t version = ReadU32(encrypted, offset);
    if (version != 1) {
        throw std::runtime_error("unsupported encrypted file version");
    }

    const uint32_t memory_kib = ReadU32(encrypted, offset);
    const uint32_t iterations = ReadU32(encrypted, offset);
    const uint32_t parallelism = ReadU32(encrypted, offset);
    const uint32_t salt_size = ReadU32(encrypted, offset);
    const uint32_t nonce_size = ReadU32(encrypted, offset);
    const uint32_t tag_size = ReadU32(encrypted, offset);
    const uint32_t ciphertext_size = ReadU32(encrypted, offset);

    if (salt_size != aes_salt_size || nonce_size != aes_nonce_size || tag_size != aes_tag_size) {
        throw std::runtime_error("encrypted file parameters are unsupported");
    }
    if (offset + salt_size + nonce_size + tag_size + ciphertext_size > encrypted.size()) {
        throw std::runtime_error("encrypted file payload is truncated");
    }

    std::array<unsigned char, aes_salt_size> salt{};
    std::array<unsigned char, aes_nonce_size> nonce{};
    std::array<unsigned char, aes_tag_size> tag{};
    std::memcpy(salt.data(), encrypted.data() + offset, salt.size());
    offset += salt.size();
    std::memcpy(nonce.data(), encrypted.data() + offset, nonce.size());
    offset += nonce.size();
    std::memcpy(tag.data(), encrypted.data() + offset, tag.size());
    offset += tag.size();
    const std::string ciphertext = encrypted.substr(offset, ciphertext_size);

    const auto key = DeriveKey(password, salt, memory_kib, iterations, parallelism);
    return AesGcmDecrypt(ciphertext, key, nonce, tag);
}

void ClearDocument(EditorState& editor) {
    editor.document.Clear();
    editor.file_path.clear();
    editor.status = "Ready";
    editor.last_action = "New document";
    editor.encrypted_document = false;
    editor.undo_stack.clear();
    editor.redo_stack.clear();
    editor.selection_start = 0;
    editor.selection_end = 0;
    editor.cursor_pos = 0;
    editor.has_selection = false;
    editor.editor_focused = false;
    editor.desired_column = 0;
    editor.mouse_selection_anchor = 0;
    editor.dragging_selection = false;
    editor.active_find_start = std::string::npos;
    editor.active_find_end = std::string::npos;
    editor.cursor_line = 1;
    editor.cursor_column = 1;
    editor.editor_scroll_y = 0.0f;
}

void ResetEditingSession(EditorState& editor) {
    editor.undo_stack.clear();
    editor.redo_stack.clear();
    editor.selection_start = 0;
    editor.selection_end = 0;
    editor.cursor_pos = 0;
    editor.has_selection = false;
    editor.editor_focused = false;
    editor.desired_column = 0;
    editor.mouse_selection_anchor = 0;
    editor.dragging_selection = false;
    editor.active_find_start = std::string::npos;
    editor.active_find_end = std::string::npos;
    editor.cursor_line = 1;
    editor.cursor_column = 1;
    editor.editor_scroll_y = 0.0f;
}

void LoadDocumentText(EditorState& editor, const std::filesystem::path& path, std::string text, bool encrypted) {
    editor.document.SetText(std::move(text));
    editor.document.MarkSaved();
    editor.file_path = path;
    editor.encrypted_document = encrypted;
    editor.status = "Ready";
    editor.last_action = "Opened " + path.filename().string();
    ResetEditingSession(editor);
}

void ResetPasswordPrompt(EditorState& editor) {
    editor.password_buffer.fill('\0');
    editor.confirm_password_buffer.fill('\0');
    editor.password_error.clear();
}

void BeginPasswordPrompt(EditorState& editor, PasswordPromptMode mode, const std::filesystem::path& path) {
    editor.password_prompt_mode = mode;
    editor.password_target_path = path;
    ResetPasswordPrompt(editor);
    editor.show_password_prompt = true;
}

bool OpenFileAtPath(EditorState& editor, const std::filesystem::path& path) {
    try {
        const std::string contents = ReadFile(path.string());
        if (IsEncryptedFileContents(contents)) {
            BeginPasswordPrompt(editor, PasswordPromptMode::Open, path);
            editor.status = "Password required";
            editor.last_action = "Open encrypted file";
            return false;
        }

        LoadDocumentText(editor, path, contents, false);
        return true;
    } catch (const std::exception& error) {
        editor.status = "Open failed: " + std::string(error.what());
        editor.last_action = "Open failed";
        return false;
    }
}

bool SaveFileAtPath(EditorState& editor, const std::filesystem::path& path) {
    try {
        WriteFile(path.string(), editor.document.text);
        editor.file_path = path;
        editor.encrypted_document = false;
        editor.status = "Ready";
        editor.last_action = "Saved " + path.filename().string();
        editor.document.MarkSaved();
        return true;
    } catch (const std::exception& error) {
        editor.status = "Save failed: " + std::string(error.what());
        editor.last_action = "Save failed";
        return false;
    }
}

bool SaveEncryptedFileAtPath(EditorState& editor, const std::filesystem::path& path, const std::string& password) {
    try {
        WriteFile(path.string(), EncryptDocumentContents(editor.document.text, password));
        editor.file_path = path;
        editor.encrypted_document = true;
        editor.status = "Ready";
        editor.last_action = "Encrypted and saved " + path.filename().string();
        editor.document.MarkSaved();
        return true;
    } catch (const std::exception& error) {
        editor.status = "Save failed: " + std::string(error.what());
        editor.last_action = "Save failed";
        return false;
    }
}

void UpdateCursorPosition(EditorState& editor, int cursor_pos) {
    const int clamped = std::clamp(cursor_pos, 0, static_cast<int>(editor.document.text.size()));
    editor.cursor_pos = clamped;
    editor.document.CursorLineColumn(static_cast<size_t>(clamped), editor.cursor_line, editor.cursor_column);
}

std::string NormalizePlainText(const char* text) {
    std::string normalized;
    if (text == nullptr) {
        return normalized;
    }

    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor == '\r') {
            if (*(cursor + 1) == '\n') {
                continue;
            }
            normalized.push_back('\n');
        } else {
            normalized.push_back(*cursor);
        }
    }

    return normalized;
}

uint32_t RandomU32() {
    uint32_t value = 0;
    FillRandom(reinterpret_cast<unsigned char*>(&value), sizeof(value));
    return value;
}

char RandomCharacterFrom(const std::string& alphabet) {
    if (alphabet.empty()) {
        throw std::runtime_error("password alphabet is empty");
    }

    const uint32_t max_value = (std::numeric_limits<uint32_t>::max)();
    const uint32_t limit = max_value - (max_value % static_cast<uint32_t>(alphabet.size()));
    uint32_t value = 0;
    do {
        value = RandomU32();
    } while (value >= limit);

    return alphabet[value % alphabet.size()];
}

std::string GeneratePassword(const EditorState& editor) {
    std::string alphabet;
    if (editor.generated_use_uppercase) {
        alphabet += "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    }
    if (editor.generated_use_lowercase) {
        alphabet += "abcdefghijklmnopqrstuvwxyz";
    }
    if (editor.generated_use_numbers) {
        alphabet += "0123456789";
    }
    if (editor.generated_use_special) {
        alphabet += "!@#$%^&*()-_=+[]{};:,.<>?";
    }

    const int length = std::clamp(editor.generated_password_length, 1, 256);
    std::string password;
    password.reserve(static_cast<size_t>(length));
    for (int i = 0; i < length; ++i) {
        password.push_back(RandomCharacterFrom(alphabet));
    }

    return password;
}

void QueueEditorCommand(EditorState& editor, EditorCommand command) {
    editor.pending_editor_command = command;
}

EditorSnapshot MakeEditorSnapshot(const EditorState& editor) {
    return EditorSnapshot{
        editor.document.text,
        editor.cursor_pos,
        editor.selection_start,
        editor.selection_end,
        editor.has_selection,
    };
}

bool SameSnapshot(const EditorSnapshot& left, const EditorSnapshot& right) {
    return left.text == right.text &&
        left.cursor_pos == right.cursor_pos &&
        left.selection_start == right.selection_start &&
        left.selection_end == right.selection_end &&
        left.has_selection == right.has_selection;
}

void PushUndoSnapshot(EditorState& editor, const EditorSnapshot& snapshot) {
    if (editor.undo_stack.empty() || !SameSnapshot(editor.undo_stack.back(), snapshot)) {
        constexpr size_t max_undo_entries = 100;
        editor.undo_stack.push_back(snapshot);
        if (editor.undo_stack.size() > max_undo_entries) {
            editor.undo_stack.erase(editor.undo_stack.begin());
        }
    }
}

void RecordUndoSnapshot(EditorState& editor) {
    PushUndoSnapshot(editor, MakeEditorSnapshot(editor));
    editor.redo_stack.clear();
}

std::pair<int, int> NormalizedSelection(const EditorState& editor) {
    int selection_start = editor.selection_start;
    int selection_end = editor.selection_end;
    if (selection_start > selection_end) {
        std::swap(selection_start, selection_end);
    }

    selection_start = std::clamp(selection_start, 0, static_cast<int>(editor.document.text.size()));
    selection_end = std::clamp(selection_end, 0, static_cast<int>(editor.document.text.size()));
    return {selection_start, selection_end};
}

bool HasSelection(const EditorState& editor) {
    const auto [selection_start, selection_end] = NormalizedSelection(editor);
    return editor.has_selection && selection_start != selection_end;
}

size_t ClampDocumentPosition(const EditorState& editor, size_t position) {
    return (std::min)(position, editor.document.text.size());
}

void ClearSelection(EditorState& editor) {
    editor.selection_start = editor.cursor_pos;
    editor.selection_end = editor.cursor_pos;
    editor.has_selection = false;
}

void SetCursorPosition(EditorState& editor, size_t position, bool preserve_desired_column = false) {
    const size_t clamped = ClampDocumentPosition(editor, position);
    UpdateCursorPosition(editor, static_cast<int>(clamped));
    if (!preserve_desired_column) {
        const auto [line, column] = editor.document.LineColumnFromPosition(clamped);
        (void)line;
        editor.desired_column = column;
    }
}

void ApplyEditorSnapshot(EditorState& editor, const EditorSnapshot& snapshot) {
    editor.document.SetText(snapshot.text);
    const int text_size = static_cast<int>(editor.document.text.size());
    editor.selection_start = std::clamp(snapshot.selection_start, 0, text_size);
    editor.selection_end = std::clamp(snapshot.selection_end, 0, text_size);
    editor.has_selection = snapshot.has_selection && editor.selection_start != editor.selection_end;
    SetCursorPosition(editor, static_cast<size_t>(std::clamp(snapshot.cursor_pos, 0, text_size)));
    if (!editor.has_selection) {
        ClearSelection(editor);
    }
}

void SetSelection(EditorState& editor, size_t anchor, size_t cursor, bool preserve_desired_column = false) {
    const size_t clamped_anchor = ClampDocumentPosition(editor, anchor);
    const size_t clamped_cursor = ClampDocumentPosition(editor, cursor);
    editor.selection_start = static_cast<int>(clamped_anchor);
    editor.selection_end = static_cast<int>(clamped_cursor);
    editor.has_selection = clamped_anchor != clamped_cursor;
    SetCursorPosition(editor, clamped_cursor, preserve_desired_column);
}

void MoveCursorTo(EditorState& editor, size_t position, bool selecting, bool preserve_desired_column = false) {
    const size_t previous_cursor = static_cast<size_t>(editor.cursor_pos);
    if (selecting) {
        const size_t anchor = editor.has_selection
            ? static_cast<size_t>(editor.selection_start)
            : previous_cursor;
        SetSelection(editor, anchor, position, preserve_desired_column);
        return;
    }

    SetCursorPosition(editor, position, preserve_desired_column);
    ClearSelection(editor);
}

bool IsUtf8Continuation(unsigned char c) {
    return (c & 0xc0) == 0x80;
}

size_t PreviousCharacterPosition(const std::string& text, size_t position) {
    if (position == 0) {
        return 0;
    }

    --position;
    while (position > 0 && IsUtf8Continuation(static_cast<unsigned char>(text[position]))) {
        --position;
    }
    return position;
}

size_t NextCharacterPosition(const std::string& text, size_t position) {
    if (position >= text.size()) {
        return text.size();
    }

    ++position;
    while (position < text.size() && IsUtf8Continuation(static_cast<unsigned char>(text[position]))) {
        ++position;
    }
    return position;
}

bool DeleteSelection(EditorState& editor) {
    if (!HasSelection(editor)) {
        return false;
    }

    const auto [selection_start, selection_end] = NormalizedSelection(editor);
    editor.document.Erase(
        static_cast<size_t>(selection_start),
        static_cast<size_t>(selection_end - selection_start));
    SetCursorPosition(editor, static_cast<size_t>(selection_start));
    ClearSelection(editor);
    editor.status = "Editing";
    editor.last_action = "Modified";
    return true;
}

void InsertTextAtCursor(EditorState& editor, const std::string& inserted_text, const char* action) {
    if (inserted_text.empty()) {
        return;
    }

    RecordUndoSnapshot(editor);
    size_t position = static_cast<size_t>(editor.cursor_pos);
    if (HasSelection(editor)) {
        const auto [selection_start, selection_end] = NormalizedSelection(editor);
        editor.document.Erase(
            static_cast<size_t>(selection_start),
            static_cast<size_t>(selection_end - selection_start));
        position = static_cast<size_t>(selection_start);
    }

    editor.document.Insert(position, inserted_text);
    SetCursorPosition(editor, position + inserted_text.size());
    ClearSelection(editor);
    editor.status = "Editing";
    editor.last_action = action;
}

void DeleteBackward(EditorState& editor) {
    if (HasSelection(editor)) {
        RecordUndoSnapshot(editor);
        DeleteSelection(editor);
        return;
    }

    if (DeleteSelection(editor)) {
        return;
    }

    const size_t cursor = static_cast<size_t>(editor.cursor_pos);
    const size_t previous = PreviousCharacterPosition(editor.document.text, cursor);
    if (previous == cursor) {
        return;
    }

    RecordUndoSnapshot(editor);
    editor.document.Erase(previous, cursor - previous);
    SetCursorPosition(editor, previous);
    ClearSelection(editor);
    editor.status = "Editing";
    editor.last_action = "Backspace";
}

void DeleteForward(EditorState& editor) {
    if (HasSelection(editor)) {
        RecordUndoSnapshot(editor);
        DeleteSelection(editor);
        return;
    }

    if (DeleteSelection(editor)) {
        return;
    }

    const size_t cursor = static_cast<size_t>(editor.cursor_pos);
    const size_t next = NextCharacterPosition(editor.document.text, cursor);
    if (next == cursor) {
        return;
    }

    RecordUndoSnapshot(editor);
    editor.document.Erase(cursor, next - cursor);
    SetCursorPosition(editor, cursor);
    ClearSelection(editor);
    editor.status = "Editing";
    editor.last_action = "Delete";
}

void CopySelection(EditorState& editor) {
    if (!HasSelection(editor)) {
        return;
    }

    const auto [selection_start, selection_end] = NormalizedSelection(editor);
    ImGui::SetClipboardText(editor.document.text.substr(
        static_cast<size_t>(selection_start),
        static_cast<size_t>(selection_end - selection_start)).c_str());
    editor.status = "Copied";
    editor.last_action = "Copied";
}

void CutSelection(EditorState& editor) {
    if (!HasSelection(editor)) {
        return;
    }

    CopySelection(editor);
    RecordUndoSnapshot(editor);
    DeleteSelection(editor);
    editor.last_action = "Cut";
}

void PasteClipboard(EditorState& editor) {
    InsertTextAtCursor(editor, NormalizePlainText(ImGui::GetClipboardText()), "Pasted");
}

bool EditorInputBlockedByDialog(const EditorState& editor) {
    return editor.show_find_window ||
        editor.show_password_generator ||
        editor.show_unsaved_prompt ||
        editor.show_password_prompt ||
        editor.password_prompt_mode != PasswordPromptMode::None ||
        ImGui::IsPopupOpen("Unsaved changes") ||
        ImGui::IsPopupOpen("File password") ||
        ImGui::IsPopupOpen("Password generator");
}

char FoldSearchChar(char value) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

bool SearchMatchesAt(const std::string& text, const std::string& needle, size_t position, bool match_case) {
    if (position + needle.size() > text.size()) {
        return false;
    }

    for (size_t i = 0; i < needle.size(); ++i) {
        const char left = text[position + i];
        const char right = needle[i];
        if (match_case ? left != right : FoldSearchChar(left) != FoldSearchChar(right)) {
            return false;
        }
    }

    return true;
}

size_t FindForwardFrom(const std::string& text, const std::string& needle, size_t start, bool match_case) {
    if (needle.empty() || text.empty() || needle.size() > text.size()) {
        return std::string::npos;
    }

    start = (std::min)(start, text.size());
    for (size_t position = start; position + needle.size() <= text.size(); ++position) {
        if (SearchMatchesAt(text, needle, position, match_case)) {
            return position;
        }
    }

    return std::string::npos;
}

size_t FindBackwardFrom(const std::string& text, const std::string& needle, size_t start, bool match_case) {
    if (needle.empty() || text.empty() || needle.size() > text.size()) {
        return std::string::npos;
    }

    size_t position = (std::min)(start, text.size() - needle.size());
    for (;;) {
        if (SearchMatchesAt(text, needle, position, match_case)) {
            return position;
        }
        if (position == 0) {
            break;
        }
        --position;
    }

    return std::string::npos;
}

void ClearActiveFindMatch(EditorState& editor) {
    editor.active_find_start = std::string::npos;
    editor.active_find_end = std::string::npos;
}

void SelectFindMatch(EditorState& editor, size_t position, size_t length, const char* action) {
    editor.active_find_start = position;
    editor.active_find_end = position + length;
    SetSelection(editor, position, position + length);
    editor.editor_focused = true;
    editor.status = "Found match";
    editor.last_action = action;
}

bool FindInDocument(EditorState& editor, bool forward) {
    const std::string needle = editor.find_buffer.data();
    if (needle.empty()) {
        ClearActiveFindMatch(editor);
        editor.show_find_window = true;
        editor.status = "Enter search text";
        editor.last_action = "Find";
        return false;
    }

    size_t found = std::string::npos;
    if (forward) {
        const size_t start = HasSelection(editor)
            ? static_cast<size_t>(NormalizedSelection(editor).second)
            : static_cast<size_t>(editor.cursor_pos);
        found = FindForwardFrom(editor.document.text, needle, start, editor.find_match_case);
        if (found == std::string::npos && editor.find_wrap) {
            found = FindForwardFrom(editor.document.text, needle, 0, editor.find_match_case);
        }
    } else {
        const size_t start = HasSelection(editor)
            ? static_cast<size_t>(NormalizedSelection(editor).first == 0 ? 0 : NormalizedSelection(editor).first - 1)
            : static_cast<size_t>(editor.cursor_pos == 0 ? 0 : editor.cursor_pos - 1);
        found = FindBackwardFrom(editor.document.text, needle, start, editor.find_match_case);
        if (found == std::string::npos && editor.find_wrap && !editor.document.text.empty()) {
            found = FindBackwardFrom(editor.document.text, needle, editor.document.text.size() - 1, editor.find_match_case);
        }
    }

    if (found == std::string::npos) {
        ClearActiveFindMatch(editor);
        editor.status = "No matches";
        editor.last_action = "Find failed";
        return false;
    }

    SelectFindMatch(editor, found, needle.size(), forward ? "Find next" : "Find previous");
    return true;
}

bool PickOpenPath(GLFWwindow* window, std::filesystem::path& path) {
    std::array<char, MAX_PATH> buffer{};
    OPENFILENAMEA dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = glfwGetWin32Window(window);
    dialog.lpstrFilter = "Text Files\0*.txt;*.md;*.cpp;*.h;*.hpp;*.c;*.json;*.log\0All Files\0*.*\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    dialog.lpstrDefExt = "txt";

    if (!GetOpenFileNameA(&dialog)) {
        return false;
    }

    path = buffer.data();
    return true;
}

bool PickSavePath(GLFWwindow* window, const std::filesystem::path& current_path, std::filesystem::path& path) {
    std::array<char, MAX_PATH> buffer{};
    if (!current_path.empty()) {
        const std::string current = current_path.string();
        std::strncpy(buffer.data(), current.c_str(), buffer.size() - 1);
    } else {
        std::strncpy(buffer.data(), "untitled.txt", buffer.size() - 1);
    }

    OPENFILENAMEA dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = glfwGetWin32Window(window);
    dialog.lpstrFilter = "Text Files\0*.txt;*.md;*.cpp;*.h;*.hpp;*.c;*.json;*.log\0All Files\0*.*\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    dialog.lpstrDefExt = "txt";

    if (!GetSaveFileNameA(&dialog)) {
        return false;
    }

    path = buffer.data();
    return true;
}

bool OpenFileFromDialog(EditorState& editor, GLFWwindow* window) {
    std::filesystem::path path;
    if (!PickOpenPath(window, path)) {
        return false;
    }

    return OpenFileAtPath(editor, path);
}

void CompletePendingAction(EditorState& editor, GLFWwindow* window);
bool SaveEncryptedFile(EditorState& editor, GLFWwindow* window, bool complete_pending_after_save = false);

bool SaveFileAs(EditorState& editor, GLFWwindow* window, bool complete_pending_after_save = false) {
    std::filesystem::path path;
    if (!PickSavePath(window, editor.file_path, path)) {
        return false;
    }

    if (SaveFileAtPath(editor, path)) {
        if (complete_pending_after_save) {
            CompletePendingAction(editor, window);
        }
        return true;
    }
    return false;
}

bool SaveFile(EditorState& editor, GLFWwindow* window, bool complete_pending_after_save = false) {
    if (editor.file_path.empty()) {
        return SaveFileAs(editor, window, complete_pending_after_save);
    }

    if (editor.encrypted_document) {
        return SaveEncryptedFile(editor, window, complete_pending_after_save);
    }

    if (SaveFileAtPath(editor, editor.file_path)) {
        if (complete_pending_after_save) {
            CompletePendingAction(editor, window);
        }
        return true;
    }
    return false;
}

bool SaveEncryptedFileAs(EditorState& editor, GLFWwindow* window, bool complete_pending_after_save = false) {
    std::filesystem::path path;
    if (!PickSavePath(window, editor.file_path, path)) {
        return false;
    }

    editor.complete_pending_after_save = complete_pending_after_save;
    BeginPasswordPrompt(editor, PasswordPromptMode::Save, path);
    return false;
}

bool SaveEncryptedFile(EditorState& editor, GLFWwindow* window, bool complete_pending_after_save) {
    if (editor.file_path.empty()) {
        return SaveEncryptedFileAs(editor, window, complete_pending_after_save);
    }

    editor.complete_pending_after_save = complete_pending_after_save;
    BeginPasswordPrompt(editor, PasswordPromptMode::Save, editor.file_path);
    return false;
}

void BeginGuardedAction(EditorState& editor, PendingAction action) {
    if (!editor.document.IsDirty()) {
        editor.pending_action = action;
        return;
    }

    editor.pending_action = action;
    editor.show_unsaved_prompt = true;
}

void CompletePendingAction(EditorState& editor, GLFWwindow* window) {
    const PendingAction action = editor.pending_action;
    editor.pending_action = PendingAction::None;

    switch (action) {
    case PendingAction::None:
        break;
    case PendingAction::NewFile:
        ClearDocument(editor);
        break;
    case PendingAction::OpenFile:
        OpenFileFromDialog(editor, window);
        break;
    case PendingAction::CloseApp:
        editor.close_after_frame = true;
        break;
    }
}

bool IsConfirmPressed() {
    return ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
}

void DrawUnsavedPrompt(EditorState& editor, GLFWwindow* window) {
    if (editor.show_unsaved_prompt) {
        ImGui::OpenPopup("Unsaved changes");
        editor.show_unsaved_prompt = false;
    }

    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::BeginPopupModal("Unsaved changes", nullptr, flags)) {
        return;
    }

    const bool appearing = ImGui::IsWindowAppearing();
    ImGui::TextUnformatted("The current file has unsaved changes.");
    ImGui::Spacing();

    const bool confirm = IsConfirmPressed();
    if (appearing) {
        ImGui::SetKeyboardFocusHere();
    }
    if (ImGui::Button("Save", ImVec2(90.0f, 0.0f)) || confirm) {
        SaveFile(editor, window, true);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard", ImVec2(90.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
        CompletePendingAction(editor, window);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) {
        editor.pending_action = PendingAction::None;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void DrawPasswordPrompt(EditorState& editor, GLFWwindow* window) {
    if (editor.show_password_prompt) {
        ImGui::OpenPopup("File password");
        editor.show_password_prompt = false;
    }

    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::BeginPopupModal("File password", nullptr, flags)) {
        return;
    }

    const bool appearing = ImGui::IsWindowAppearing();
    const bool is_save = editor.password_prompt_mode == PasswordPromptMode::Save;
    ImGui::TextUnformatted(is_save ? "Encrypt file with password" : "Enter file password");
    ImGui::TextDisabled("%s", editor.password_target_path.filename().string().c_str());
    ImGui::Spacing();

    if (appearing) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputText("Password", editor.password_buffer.data(), editor.password_buffer.size(), ImGuiInputTextFlags_Password);
    if (is_save) {
        ImGui::InputText(
            "Confirm",
            editor.confirm_password_buffer.data(),
            editor.confirm_password_buffer.size(),
            ImGuiInputTextFlags_Password);
    }

    if (!editor.password_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", editor.password_error.c_str());
    }

    ImGui::Spacing();
    const bool confirm = IsConfirmPressed();
    if (ImGui::Button(is_save ? "Encrypt Save" : "Open", ImVec2(110.0f, 0.0f)) || confirm) {
        const std::string password = editor.password_buffer.data();
        if (password.empty()) {
            editor.password_error = "Password cannot be empty.";
        } else if (is_save && password != editor.confirm_password_buffer.data()) {
            editor.password_error = "Passwords do not match.";
        } else {
            try {
                if (is_save) {
                    if (SaveEncryptedFileAtPath(editor, editor.password_target_path, password)) {
                        const bool continue_pending = editor.complete_pending_after_save;
                        editor.complete_pending_after_save = false;
                        editor.password_prompt_mode = PasswordPromptMode::None;
                        ImGui::CloseCurrentPopup();
                        if (continue_pending) {
                            CompletePendingAction(editor, window);
                        }
                    }
                } else {
                    const std::string encrypted = ReadFile(editor.password_target_path.string());
                    LoadDocumentText(editor, editor.password_target_path, DecryptDocumentContents(encrypted, password), true);
                    editor.password_prompt_mode = PasswordPromptMode::None;
                    ImGui::CloseCurrentPopup();
                }
            } catch (const std::exception& error) {
                editor.password_error = error.what();
                editor.status = is_save
                    ? "Save failed: " + editor.password_error
                    : "Open failed: " + editor.password_error;
                editor.last_action = is_save ? "Save failed" : "Open failed";
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) {
        editor.complete_pending_after_save = false;
        editor.pending_action = PendingAction::None;
        editor.password_prompt_mode = PasswordPromptMode::None;
        editor.password_error.clear();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void DrawPasswordGenerator(EditorState& editor) {
    if (editor.show_password_generator) {
        ImGui::OpenPopup("Password generator");
        editor.show_password_generator = false;
    }

    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::BeginPopupModal("Password generator", nullptr, flags)) {
        return;
    }

    const bool appearing = ImGui::IsWindowAppearing();
    ImGui::SetNextItemWidth(120.0f);
    if (appearing) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputInt("Length", &editor.generated_password_length);
    editor.generated_password_length = std::clamp(editor.generated_password_length, 1, 256);
    ImGui::Checkbox("Capitals", &editor.generated_use_uppercase);
    ImGui::Checkbox("Lowercase", &editor.generated_use_lowercase);
    ImGui::Checkbox("Numbers", &editor.generated_use_numbers);
    ImGui::Checkbox("Special characters", &editor.generated_use_special);

    ImGui::Spacing();
    const bool confirm = IsConfirmPressed();
    if (ImGui::Button("Generate", ImVec2(100.0f, 0.0f)) || confirm) {
        try {
            const std::string password = GeneratePassword(editor);
            ImGui::SetClipboardText(password.c_str());
            editor.pending_insert_text = password;
            QueueEditorCommand(editor, EditorCommand::InsertText);
            editor.status = "Generated password";
            editor.last_action = "Generated password";
            ImGui::CloseCurrentPopup();
        } catch (const std::exception& error) {
            editor.status = "Password generation failed: " + std::string(error.what());
            editor.last_action = "Generation failed";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

float CalcLogicalLineHeight(const char* line_begin, const char* line_end, float wrap_width) {
    const float line_height = ImGui::GetTextLineHeight();
    if (line_begin == line_end || wrap_width <= 0.0f) {
        return line_height;
    }

    float height = 0.0f;
    const char* cursor = line_begin;
    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize();

    while (cursor < line_end) {
        const char* wrap_end = ImFontCalcWordWrapPositionEx(font, font_size, cursor, line_end, wrap_width);
        if (wrap_end <= cursor) {
            wrap_end = cursor + 1;
        }
        height += line_height;
        cursor = wrap_end;

        while (cursor < line_end && (*cursor == ' ' || *cursor == '\t')) {
            ++cursor;
        }
    }

    return height;
}

void EnsureVisualLineMetrics(EditorState& editor, float wrap_width) {
    const bool wrap_enabled = editor.word_wrap && wrap_width > 0.0f;
    const bool cache_valid =
        editor.visual_metrics_revision == editor.document.edit_revision &&
        editor.visual_metrics_word_wrap == wrap_enabled &&
        std::fabs(editor.visual_metrics_wrap_width - wrap_width) < 0.5f;

    if (cache_valid) {
        return;
    }

    const int line_count = editor.document.LineCount();
    const float line_height = ImGui::GetTextLineHeight();
    editor.visual_line_offsets.assign(static_cast<size_t>(line_count) + 1, 0.0f);
    editor.visual_line_heights.assign(static_cast<size_t>(line_count), line_height);

    const char* text_begin = editor.document.text.c_str();
    for (int line = 0; line < line_count; ++line) {
        const size_t start = editor.document.line_offsets[static_cast<size_t>(line)];
        size_t end = editor.document.text.size();
        if (line + 1 < line_count) {
            end = editor.document.line_offsets[static_cast<size_t>(line + 1)] - 1;
        }

        const float logical_height = wrap_enabled
            ? CalcLogicalLineHeight(text_begin + start, text_begin + end, wrap_width)
            : line_height;
        editor.visual_line_heights[static_cast<size_t>(line)] = logical_height;
        editor.visual_line_offsets[static_cast<size_t>(line + 1)] =
            editor.visual_line_offsets[static_cast<size_t>(line)] + logical_height;
    }

    editor.visual_metrics_revision = editor.document.edit_revision;
    editor.visual_metrics_wrap_width = wrap_width;
    editor.visual_metrics_word_wrap = wrap_enabled;
}

void DrawLineNumberGutter(EditorState& editor, ImVec2 pos, float height, float width, float wrap_width) {
    EnsureVisualLineMetrics(editor, wrap_width);

    const ImVec2 size(width, height);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImU32 bg = ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
    const ImU32 border = ImGui::GetColorU32(ImGuiCol_Border);
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const float line_height = ImGui::GetTextLineHeight();
    const float padding_x = ImGui::GetStyle().FramePadding.x;
    const float frame_padding_y = ImGui::GetStyle().FramePadding.y;

    draw_list->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg);
    draw_list->AddLine(
        ImVec2(pos.x + size.x - 1.0f, pos.y),
        ImVec2(pos.x + size.x - 1.0f, pos.y + size.y),
        border);

    const float top = pos.y;
    const float bottom = pos.y + height;
    const float first_visible_offset = (std::max)(0.0f, editor.editor_scroll_y - frame_padding_y);
    auto first_visible = std::upper_bound(
        editor.visual_line_offsets.begin(),
        editor.visual_line_offsets.end(),
        first_visible_offset);
    size_t line_index = first_visible == editor.visual_line_offsets.begin()
        ? 0
        : static_cast<size_t>(std::distance(editor.visual_line_offsets.begin(), first_visible) - 1);

    draw_list->PushClipRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), true);

    while (line_index < editor.visual_line_heights.size()) {
        const float screen_y = top + frame_padding_y + editor.visual_line_offsets[line_index] - editor.editor_scroll_y;
        if (screen_y + line_height >= top && screen_y <= bottom) {
            const std::string number = std::to_string(line_index + 1);
            const ImVec2 number_size = ImGui::CalcTextSize(number.c_str());
            const float x = pos.x + width - padding_x - number_size.x - 2.0f;
            draw_list->AddText(ImVec2(x, screen_y), text, number.c_str());
        }

        if (screen_y > bottom) {
            break;
        }

        ++line_index;
    }

    draw_list->PopClipRect();
}

std::vector<VisualRow> BuildVisualRows(const Document& document, bool word_wrap, float wrap_width, float line_height) {
    std::vector<VisualRow> rows;
    rows.reserve(document.lines.size());

    float y = 0.0f;
    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize();

    for (int line_index = 0; line_index < static_cast<int>(document.lines.size()); ++line_index) {
        const std::string& line = document.lines[static_cast<size_t>(line_index)];
        if (line.empty() || !word_wrap || wrap_width <= 1.0f) {
            rows.push_back(VisualRow{line_index, 0, line.size(), y});
            y += line_height;
            continue;
        }

        const char* line_begin = line.data();
        const char* cursor = line_begin;
        const char* line_end = line_begin + line.size();
        while (cursor < line_end) {
            const char* wrap_end = ImFontCalcWordWrapPositionEx(font, font_size, cursor, line_end, wrap_width);
            if (wrap_end <= cursor) {
                wrap_end = cursor + 1;
            }

            rows.push_back(VisualRow{
                line_index,
                static_cast<size_t>(cursor - line_begin),
                static_cast<size_t>(wrap_end - line_begin),
                y,
            });
            y += line_height;
            cursor = wrap_end;

            while (cursor < line_end && (*cursor == ' ' || *cursor == '\t')) {
                ++cursor;
            }
        }
    }

    if (rows.empty()) {
        rows.push_back(VisualRow{0, 0, 0, 0.0f});
    }
    return rows;
}

float MeasureTextRange(const std::string& text, size_t start, size_t end) {
    start = (std::min)(start, text.size());
    end = (std::min)(end, text.size());
    if (end <= start) {
        return 0.0f;
    }

    return ImGui::CalcTextSize(text.data() + start, text.data() + end).x;
}

size_t ColumnFromVisualX(const std::string& line, const VisualRow& row, float x) {
    if (x <= 0.0f || row.end <= row.start) {
        return row.start;
    }

    size_t best_column = row.start;
    float best_distance = (std::numeric_limits<float>::max)();
    for (size_t column = row.start; column <= row.end; column = NextCharacterPosition(line, column)) {
        const float width = MeasureTextRange(line, row.start, column);
        const float distance = std::fabs(width - x);
        if (distance < best_distance) {
            best_distance = distance;
            best_column = column;
        }
        if (width > x && column > row.start) {
            break;
        }
        if (column == row.end) {
            break;
        }
    }

    return best_column;
}

size_t PositionFromMouse(const EditorState& editor, const std::vector<VisualRow>& rows, ImVec2 origin, float gutter_width, float line_height) {
    if (rows.empty()) {
        return 0;
    }

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float local_y = mouse.y - origin.y;
    int row_index = 0;
    if (local_y > 0.0f) {
        row_index = static_cast<int>(local_y / line_height);
    }
    row_index = std::clamp(row_index, 0, static_cast<int>(rows.size()) - 1);

    const VisualRow& row = rows[static_cast<size_t>(row_index)];
    const std::string& line = editor.document.lines[static_cast<size_t>(row.line)];
    const float local_x = mouse.x - (origin.x + gutter_width);
    const size_t column = ColumnFromVisualX(line, row, local_x);
    return editor.document.PositionFromLineColumn(static_cast<size_t>(row.line), column);
}

size_t CursorVisualRowIndex(const EditorState& editor, const std::vector<VisualRow>& rows) {
    const auto [line_index, column] = editor.document.LineColumnFromPosition(static_cast<size_t>(editor.cursor_pos));
    for (size_t row_index = 0; row_index < rows.size(); ++row_index) {
        const VisualRow& row = rows[row_index];
        if (static_cast<size_t>(row.line) != line_index) {
            continue;
        }
        if (column >= row.start && column <= row.end) {
            return row_index;
        }
    }

    return rows.empty() ? 0 : rows.size() - 1;
}

void EnsureCursorVisible(const EditorState& editor, const std::vector<VisualRow>& rows, float line_height, float padding_y) {
    if (rows.empty()) {
        return;
    }

    const VisualRow& row = rows[CursorVisualRowIndex(editor, rows)];
    const float scroll_y = ImGui::GetScrollY();
    const float visible_height = ImGui::GetWindowHeight();
    if (row.y < scroll_y) {
        ImGui::SetScrollY((std::max)(0.0f, row.y));
    } else if (row.y + line_height + padding_y * 2.0f > scroll_y + visible_height) {
        ImGui::SetScrollY(row.y + line_height + padding_y * 2.0f - visible_height);
    }
}

void DrawSelectionForRow(
    const EditorState& editor,
    const VisualRow& row,
    ImDrawList* draw_list,
    ImVec2 text_pos,
    float line_height) {
    if (!HasSelection(editor)) {
        return;
    }

    const auto [selection_start, selection_end] = NormalizedSelection(editor);
    const size_t row_start = editor.document.PositionFromLineColumn(static_cast<size_t>(row.line), row.start);
    const size_t row_end = editor.document.PositionFromLineColumn(static_cast<size_t>(row.line), row.end);
    const size_t highlight_start = (std::max)(static_cast<size_t>(selection_start), row_start);
    const size_t highlight_end = (std::min)(static_cast<size_t>(selection_end), row_end);
    if (highlight_start >= highlight_end) {
        return;
    }

    const std::string& line = editor.document.lines[static_cast<size_t>(row.line)];
    const size_t line_start = editor.document.LineStartByIndex(static_cast<size_t>(row.line));
    const size_t start_column = highlight_start - line_start;
    const size_t end_column = highlight_end - line_start;
    const float start_x = text_pos.x + MeasureTextRange(line, row.start, start_column);
    const float end_x = text_pos.x + MeasureTextRange(line, row.start, end_column);
    draw_list->AddRectFilled(
        ImVec2(start_x, text_pos.y),
        ImVec2((std::max)(start_x + 1.0f, end_x), text_pos.y + line_height),
        ImGui::GetColorU32(ImGuiCol_TextSelectedBg));
}

void DrawFindHighlightsForRow(
    const EditorState& editor,
    const VisualRow& row,
    ImDrawList* draw_list,
    ImVec2 text_pos,
    float line_height) {
    const std::string needle = editor.find_buffer.data();
    if (needle.empty()) {
        return;
    }

    if (row.start >= row.end) {
        return;
    }

    const std::string& line = editor.document.lines[static_cast<size_t>(row.line)];
    const ImU32 color = ImGui::GetColorU32(ImVec4(0.85f, 0.65f, 0.18f, 0.35f));

    size_t match = FindForwardFrom(line, needle, row.start, editor.find_match_case);
    while (match != std::string::npos && match < row.end) {
        const size_t match_end = match + needle.size();
        const size_t highlight_start = (std::max)(match, row.start);
        const size_t highlight_end = (std::min)(match_end, row.end);
        if (highlight_start < highlight_end) {
            const float start_x = text_pos.x + MeasureTextRange(line, row.start, highlight_start);
            const float end_x = text_pos.x + MeasureTextRange(line, row.start, highlight_end);
            draw_list->AddRectFilled(
                ImVec2(start_x, text_pos.y),
                ImVec2((std::max)(start_x + 1.0f, end_x), text_pos.y + line_height),
                color);
        }

        match = FindForwardFrom(line, needle, match + 1, editor.find_match_case);
    }
}

void MoveCursorVertical(EditorState& editor, int delta, bool selecting) {
    const auto [line_index, column] = editor.document.LineColumnFromPosition(static_cast<size_t>(editor.cursor_pos));
    if (editor.desired_column == 0 && column != 0) {
        editor.desired_column = column;
    }

    const int target_line = std::clamp(
        static_cast<int>(line_index) + delta,
        0,
        (std::max)(0, editor.document.LineCount() - 1));
    const size_t target = editor.document.PositionFromLineColumn(
        static_cast<size_t>(target_line),
        editor.desired_column);
    MoveCursorTo(editor, target, selecting, true);
}

void HandleCustomEditorKeyboard(EditorState& editor) {
    if (!editor.editor_focused || EditorInputBlockedByDialog(editor) || ImGui::IsAnyItemActive()) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    const bool selecting = io.KeyShift;

    if (io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            CopySelection(editor);
        } else if (ImGui::IsKeyPressed(ImGuiKey_X, false)) {
            CutSelection(editor);
        } else if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
            PasteClipboard(editor);
        }
        return;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
        MoveCursorTo(editor, PreviousCharacterPosition(editor.document.text, static_cast<size_t>(editor.cursor_pos)), selecting);
    } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
        MoveCursorTo(editor, NextCharacterPosition(editor.document.text, static_cast<size_t>(editor.cursor_pos)), selecting);
    } else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
        MoveCursorVertical(editor, -1, selecting);
    } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
        MoveCursorVertical(editor, 1, selecting);
    } else if (ImGui::IsKeyPressed(ImGuiKey_Home, true)) {
        const auto [line_index, column] = editor.document.LineColumnFromPosition(static_cast<size_t>(editor.cursor_pos));
        (void)column;
        MoveCursorTo(editor, editor.document.PositionFromLineColumn(line_index, 0), selecting);
    } else if (ImGui::IsKeyPressed(ImGuiKey_End, true)) {
        const auto [line_index, column] = editor.document.LineColumnFromPosition(static_cast<size_t>(editor.cursor_pos));
        (void)column;
        MoveCursorTo(
            editor,
            editor.document.PositionFromLineColumn(line_index, editor.document.lines[line_index].size()),
            selecting);
    } else if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true)) {
        DeleteBackward(editor);
    } else if (ImGui::IsKeyPressed(ImGuiKey_Delete, true)) {
        DeleteForward(editor);
    } else if (ImGui::IsKeyPressed(ImGuiKey_Enter, true) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, true)) {
        InsertTextAtCursor(editor, "\n", "Inserted newline");
    } else if (ImGui::IsKeyPressed(ImGuiKey_Tab, true)) {
        InsertTextAtCursor(editor, "    ", "Inserted tab");
    }

    std::string typed;
    for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
        const ImWchar character = io.InputQueueCharacters[i];
        if (character < 32 || character == 127) {
            continue;
        }

        char buffer[5] = {};
        const int length = ImTextCharToUtf8(buffer, static_cast<unsigned int>(character));
        if (length > 0) {
            typed.append(buffer, static_cast<size_t>(length));
        }
    }

    if (!typed.empty()) {
        InsertTextAtCursor(editor, typed, "Typed");
        io.InputQueueCharacters.resize(0);
    }
}

void DrawCustomEditorSurface(EditorState& editor, ImVec2 size, float gutter_width) {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImVec2 resolved_size = size;
    const ImVec2 available_region = ImGui::GetContentRegionAvail();
    if (resolved_size.x <= 0.0f) {
        resolved_size.x = available_region.x;
    }
    if (resolved_size.y <= 0.0f) {
        resolved_size.y = available_region.y;
    }

    const float line_height = ImGui::GetTextLineHeight();
    const float padding_x = style.FramePadding.x;
    const float padding_y = style.FramePadding.y;
    const float available_width = (std::max)(1.0f, resolved_size.x - gutter_width - padding_x * 2.0f - style.ScrollbarSize);
    std::vector<VisualRow> rows = BuildVisualRows(editor.document, editor.word_wrap, available_width, line_height);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
    ImGui::BeginChild("##custom_text_editor", resolved_size, false, ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_HorizontalScrollbar);

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 origin(
        window->Pos.x + padding_x - ImGui::GetScrollX(),
        window->Pos.y + padding_y - ImGui::GetScrollY());
    float scroll_y = ImGui::GetScrollY();
    float visible_bottom = scroll_y + ImGui::GetWindowHeight();
    const ImU32 text_color = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 gutter_text_color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 border_color = ImGui::GetColorU32(ImGuiCol_Border);
    const size_t revision_before_input = editor.document.edit_revision;

    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hovered) {
            editor.editor_focused = true;
            const size_t clicked_position = PositionFromMouse(editor, rows, origin, gutter_width, line_height);
            if (ImGui::GetIO().KeyShift) {
                SetSelection(editor, static_cast<size_t>(editor.cursor_pos), clicked_position);
            } else {
                MoveCursorTo(editor, clicked_position, false);
                editor.mouse_selection_anchor = clicked_position;
            }
            editor.dragging_selection = true;
        } else if (!ImGui::IsAnyItemHovered()) {
            editor.editor_focused = false;
        }
    }

    if (editor.dragging_selection && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const size_t drag_position = PositionFromMouse(editor, rows, origin, gutter_width, line_height);
        SetSelection(editor, editor.mouse_selection_anchor, drag_position);
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        editor.dragging_selection = false;
    }

    HandleCustomEditorKeyboard(editor);
    if (editor.document.edit_revision != revision_before_input) {
        rows = BuildVisualRows(editor.document, editor.word_wrap, available_width, line_height);
    }

    EnsureCursorVisible(editor, rows, line_height, padding_y);
    origin = ImVec2(
        window->Pos.x + padding_x - ImGui::GetScrollX(),
        window->Pos.y + padding_y - ImGui::GetScrollY());
    scroll_y = ImGui::GetScrollY();
    visible_bottom = scroll_y + ImGui::GetWindowHeight();
    const float content_height = rows.empty() ? line_height : rows.back().y + line_height + padding_y * 2.0f;

    draw_list->PushClipRect(window->InnerClipRect.Min, window->InnerClipRect.Max, true);

    if (editor.show_line_numbers) {
        const float gutter_right = window->Pos.x + gutter_width;
        draw_list->AddLine(ImVec2(gutter_right - 1.0f, window->InnerClipRect.Min.y), ImVec2(gutter_right - 1.0f, window->InnerClipRect.Max.y), border_color);
    }

    auto first_row = std::lower_bound(
        rows.begin(),
        rows.end(),
        (std::max)(0.0f, scroll_y - line_height),
        [](const VisualRow& row, float y) {
            return row.y < y;
        });

    int last_drawn_line = -1;
    for (auto it = first_row; it != rows.end(); ++it) {
        if (it->y > visible_bottom) {
            break;
        }

        const std::string& line = editor.document.lines[static_cast<size_t>(it->line)];
        const float screen_y = origin.y + it->y;
        if (editor.show_line_numbers && it->line != last_drawn_line) {
            const std::string number = std::to_string(it->line + 1);
            const ImVec2 number_size = ImGui::CalcTextSize(number.c_str());
            draw_list->AddText(
                ImVec2(window->Pos.x + gutter_width - padding_x - number_size.x - 2.0f, screen_y),
                gutter_text_color,
                number.c_str());
            last_drawn_line = it->line;
        }

        const char* text_begin = line.data() + it->start;
        const char* text_end = line.data() + it->end;
        const ImVec2 text_pos(origin.x + gutter_width, screen_y);
        DrawFindHighlightsForRow(editor, *it, draw_list, text_pos, line_height);
        DrawSelectionForRow(editor, *it, draw_list, text_pos, line_height);
        draw_list->AddText(text_pos, text_color, text_begin, text_end);
    }

    if (editor.editor_focused && rows.size() > 0) {
        const size_t cursor_row_index = CursorVisualRowIndex(editor, rows);
        const VisualRow& cursor_row = rows[cursor_row_index];
        const auto [line_index, column] = editor.document.LineColumnFromPosition(static_cast<size_t>(editor.cursor_pos));
        const std::string& line = editor.document.lines[line_index];
        const float cursor_x = origin.x + gutter_width + MeasureTextRange(line, cursor_row.start, column);
        const float cursor_y = origin.y + cursor_row.y;
        const bool cursor_visible = std::fmod(static_cast<float>(ImGui::GetTime()), 1.2f) < 0.8f;
        if (cursor_visible && cursor_y + line_height >= window->InnerClipRect.Min.y && cursor_y <= window->InnerClipRect.Max.y) {
            draw_list->AddLine(
                ImVec2(cursor_x, cursor_y),
                ImVec2(cursor_x, cursor_y + line_height),
                text_color,
                1.0f);
        }
    }

    draw_list->PopClipRect();

    const float content_width = editor.word_wrap
        ? (std::max)(available_width + gutter_width, ImGui::GetContentRegionAvail().x)
        : 2400.0f;
    ImGui::Dummy(ImVec2(content_width, content_height));
    editor.editor_scroll_y = scroll_y;

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void ExecuteEditorCommand(EditorState& editor) {
    const EditorCommand command = editor.pending_editor_command;
    editor.pending_editor_command = EditorCommand::None;
    if (command == EditorCommand::None) {
        return;
    }

    switch (command) {
    case EditorCommand::None:
        break;
    case EditorCommand::Undo:
        if (!editor.undo_stack.empty()) {
            editor.redo_stack.push_back(MakeEditorSnapshot(editor));
            EditorSnapshot previous = std::move(editor.undo_stack.back());
            editor.undo_stack.pop_back();
            ApplyEditorSnapshot(editor, previous);
            editor.status = "Editing";
        }
        editor.last_action = "Undo";
        break;
    case EditorCommand::Redo:
        if (!editor.redo_stack.empty()) {
            PushUndoSnapshot(editor, MakeEditorSnapshot(editor));
            EditorSnapshot next = std::move(editor.redo_stack.back());
            editor.redo_stack.pop_back();
            ApplyEditorSnapshot(editor, next);
            editor.status = "Editing";
        }
        editor.last_action = "Redo";
        break;
    case EditorCommand::Cut:
        CutSelection(editor);
        break;
    case EditorCommand::Copy:
        CopySelection(editor);
        break;
    case EditorCommand::Paste:
        PasteClipboard(editor);
        break;
    case EditorCommand::InsertText:
        InsertTextAtCursor(editor, editor.pending_insert_text, "Inserted generated password");
        editor.pending_insert_text.clear();
        break;
    case EditorCommand::SelectAll:
        editor.selection_start = 0;
        editor.selection_end = static_cast<int>(editor.document.text.size());
        editor.has_selection = !editor.document.text.empty();
        SetCursorPosition(editor, editor.document.text.size());
        editor.status = "Selected all";
        editor.last_action = "Selected all";
        break;
    case EditorCommand::FindNext:
        FindInDocument(editor, true);
        break;
    case EditorCommand::FindPrevious:
        FindInDocument(editor, false);
        break;
    }
}

void DrawMenuBar(EditorState& editor, GLFWwindow* window) {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New", "Ctrl+N")) {
            BeginGuardedAction(editor, PendingAction::NewFile);
        }
        if (ImGui::MenuItem("Open", "Ctrl+O")) {
            BeginGuardedAction(editor, PendingAction::OpenFile);
        }
        if (ImGui::MenuItem("Save", "Ctrl+S")) {
            SaveFile(editor, window);
        }
        if (ImGui::MenuItem("Save As", "Ctrl+Shift+S")) {
            SaveFileAs(editor, window);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save Encrypted")) {
            SaveEncryptedFile(editor, window);
        }
        if (ImGui::MenuItem("Save Encrypted As", "Ctrl+Alt+S")) {
            SaveEncryptedFileAs(editor, window);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z")) {
            QueueEditorCommand(editor, EditorCommand::Undo);
        }
        if (ImGui::MenuItem("Redo", "Ctrl+Y / Ctrl+Shift+Z")) {
            QueueEditorCommand(editor, EditorCommand::Redo);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Cut", "Ctrl+X")) {
            QueueEditorCommand(editor, EditorCommand::Cut);
        }
        if (ImGui::MenuItem("Copy", "Ctrl+C")) {
            QueueEditorCommand(editor, EditorCommand::Copy);
        }
        if (ImGui::MenuItem("Paste as Plain Text", "Ctrl+V")) {
            QueueEditorCommand(editor, EditorCommand::Paste);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Select All", "Ctrl+A")) {
            QueueEditorCommand(editor, EditorCommand::SelectAll);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Find", "Ctrl+F")) {
            editor.show_find_window = true;
        }
        if (ImGui::MenuItem("Find Next", "F3")) {
            QueueEditorCommand(editor, EditorCommand::FindNext);
        }
        if (ImGui::MenuItem("Find Previous", "Shift+F3")) {
            QueueEditorCommand(editor, EditorCommand::FindPrevious);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Options")) {
        ImGui::MenuItem("Word wrap", nullptr, &editor.word_wrap);
        ImGui::MenuItem("Line numbers", nullptr, &editor.show_line_numbers);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Tools")) {
        if (ImGui::MenuItem("Generate Password", "Ctrl+P")) {
            editor.show_password_generator = true;
        }
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

void HandleShortcuts(EditorState& editor, GLFWwindow* window) {
    if (EditorInputBlockedByDialog(editor)) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    if (!io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) {
            QueueEditorCommand(editor, io.KeyShift ? EditorCommand::FindPrevious : EditorCommand::FindNext);
        }
        return;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_N, false)) {
        BeginGuardedAction(editor, PendingAction::NewFile);
    } else if (ImGui::IsKeyPressed(ImGuiKey_O, false)) {
        BeginGuardedAction(editor, PendingAction::OpenFile);
    } else if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        SaveEncryptedFileAs(editor, window);
    } else if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        SaveFileAs(editor, window);
    } else if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        SaveFile(editor, window);
    } else if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        QueueEditorCommand(editor, io.KeyShift ? EditorCommand::Redo : EditorCommand::Undo);
    } else if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
        QueueEditorCommand(editor, EditorCommand::Redo);
    } else if (ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        QueueEditorCommand(editor, EditorCommand::SelectAll);
    } else if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        editor.show_find_window = true;
    } else if (ImGui::IsKeyPressed(ImGuiKey_P, false)) {
        editor.show_password_generator = true;
    }
}

void DrawFindWindow(EditorState& editor) {
    if (!editor.show_find_window) {
        return;
    }

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 360.0f, viewport->WorkPos.y + 36.0f),
        ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(340.0f, 0.0f), ImGuiCond_Appearing);

    bool open = editor.show_find_window;
    if (!ImGui::Begin("Find", &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
        editor.show_find_window = open;
        ImGui::End();
        return;
    }

    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }

    const bool submitted = ImGui::InputText(
        "Text",
        editor.find_buffer.data(),
        editor.find_buffer.size(),
        ImGuiInputTextFlags_EnterReturnsTrue);

    ImGui::Checkbox("Match case", &editor.find_match_case);
    ImGui::SameLine();
    ImGui::Checkbox("Wrap", &editor.find_wrap);

    if (submitted) {
        QueueEditorCommand(editor, ImGui::GetIO().KeyShift ? EditorCommand::FindPrevious : EditorCommand::FindNext);
    }

    if (ImGui::Button("Previous")) {
        QueueEditorCommand(editor, EditorCommand::FindPrevious);
    }
    ImGui::SameLine();
    if (ImGui::Button("Next")) {
        QueueEditorCommand(editor, EditorCommand::FindNext);
    }
    ImGui::SameLine();
    if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        open = false;
        ClearActiveFindMatch(editor);
    }

    editor.show_find_window = open;
    ImGui::End();
}

void DrawEditor(EditorState& editor) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    constexpr ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("Editor", nullptr, window_flags);

    const int line_count = editor.document.LineCount();
    const int digits = (std::max)(2, static_cast<int>(std::to_string(line_count).size()));
    const float gutter_width = editor.show_line_numbers
        ? ImGui::CalcTextSize("0").x * static_cast<float>(digits) + ImGui::GetStyle().FramePadding.x * 2.0f + 10.0f
        : 0.0f;
    const float editor_height = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing();

    ExecuteEditorCommand(editor);
    DrawCustomEditorSurface(editor, ImVec2(-1.0f, editor_height), gutter_width);

    const std::string name = editor.file_path.empty()
        ? std::string("Untitled")
        : editor.file_path.filename().string();
    ImGui::Text("%s%s", name.c_str(), editor.document.IsDirty() ? " *" : "");
    ImGui::SameLine();
    ImGui::TextDisabled("| %s", editor.last_action.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("| Ln %d, Col %d", editor.cursor_line, editor.cursor_column);
    ImGui::SameLine();
    ImGui::TextDisabled("| %d lines", editor.document.LineCount());
    ImGui::SameLine();
    ImGui::TextDisabled("| %zu bytes", editor.document.text.size());
    ImGui::SameLine();
    ImGui::TextDisabled("| %s", editor.word_wrap ? "Wrap" : "No wrap");
    ImGui::SameLine();
    ImGui::TextDisabled("| %s", editor.show_line_numbers ? "# Lines" : "No # lines");
    ImGui::SameLine();
    ImGui::TextDisabled("| %s", editor.encrypted_document ? "Encrypted" : "Plaintext");
    if (editor.status != "Ready" && editor.status != "Editing") {
        ImGui::SameLine();
        ImGui::TextDisabled("| %s", editor.status.c_str());
    }

    ImGui::End();
}

void LoadEditorFont() {
    ImGuiIO& io = ImGui::GetIO();
    const std::filesystem::path font_path = "assets/fonts/ProggyClean.ttf";

    if (std::filesystem::exists(font_path)) {
        io.Fonts->AddFontFromFileTTF(font_path.string().c_str(), 13.0f);
    } else {
        io.Fonts->AddFontDefault();
    }
}

void ApplyStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.02f, 0.02f, 0.02f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.03f, 0.03f, 0.03f, 1.0f);
    style.WindowRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.TabRounding = 0.0f;
}

void SetWindowIcon(GLFWwindow* window) {
    constexpr int size = 32;
    std::array<unsigned char, size * size * 4> pixels{};

    auto set_pixel = [&](int x, int y, unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
        if (x < 0 || x >= size || y < 0 || y >= size) {
            return;
        }
        const size_t index = static_cast<size_t>((y * size + x) * 4);
        pixels[index + 0] = r;
        pixels[index + 1] = g;
        pixels[index + 2] = b;
        pixels[index + 3] = a;
    };

    auto fill_rect = [&](int x, int y, int width, int height, unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
        for (int yy = y; yy < y + height; ++yy) {
            for (int xx = x; xx < x + width; ++xx) {
                set_pixel(xx, yy, r, g, b, a);
            }
        }
    };

    fill_rect(8, 6, 16, 22, 0, 0, 0, 110);
    fill_rect(6, 4, 18, 22, 245, 245, 238, 255);
    fill_rect(22, 6, 2, 20, 185, 185, 178, 255);
    fill_rect(8, 26, 16, 2, 155, 155, 150, 255);
    fill_rect(6, 4, 18, 4, 75, 148, 210, 255);
    fill_rect(6, 8, 18, 2, 42, 92, 145, 255);

    for (int x : {8, 12, 16, 20}) {
        fill_rect(x, 4, 2, 2, 28, 28, 28, 255);
    }

    fill_rect(10, 12, 10, 2, 55, 55, 55, 255);
    fill_rect(10, 16, 8, 2, 55, 55, 55, 255);
    fill_rect(10, 20, 10, 2, 55, 55, 55, 255);

    GLFWimage image{
        size,
        size,
        pixels.data(),
    };
    glfwSetWindowIcon(window, 1, &image);
}

} // namespace

int main() {
    const AppConfig config = LoadConfig();

    if (!glfwInit()) {
        return 1;
    }

    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow* window = glfwCreateWindow(
        config.window_width,
        config.window_height,
        "formyself",
        nullptr,
        nullptr);
    if (window == nullptr) {
        glfwTerminate();
        return 1;
    }
    SetWindowIcon(window);

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    LoadEditorFont();
    ApplyStyle();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    EditorState editor;
    ApplyConfig(editor, config);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        HandleShortcuts(editor, window);
        DrawMenuBar(editor, window);
        if (editor.pending_action != PendingAction::None && !editor.show_unsaved_prompt && !editor.document.IsDirty()) {
            CompletePendingAction(editor, window);
        }
        DrawEditor(editor);
        DrawFindWindow(editor);
        DrawUnsavedPrompt(editor, window);
        DrawPasswordPrompt(editor, window);
        DrawPasswordGenerator(editor);

        if (glfwWindowShouldClose(window) && editor.document.IsDirty() && editor.pending_action != PendingAction::CloseApp) {
            glfwSetWindowShouldClose(window, GLFW_FALSE);
            BeginGuardedAction(editor, PendingAction::CloseApp);
        }

        std::string title = editor.file_path.filename().string();
        if (title.empty()) {
            title = "Untitled";
        }
        if (editor.document.IsDirty()) {
            title += " *";
        }
        title += " - formyself";
        glfwSetWindowTitle(window, title.c_str());

        ImGui::Render();
        int display_w = 0;
        int display_h = 0;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.07f, 0.07f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);

        if (editor.close_after_frame) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
    }

    try {
        SaveConfig(editor, window);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Failed to save editor_config.ini: %s\n", error.what());
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
