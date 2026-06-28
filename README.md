# Dear ImGui Text Editor

A small direct Dear ImGui text editor with:

- New, Open, Save, and Save As
- Native Windows file dialogs
- Dirty marker in the title
- Optional encrypted saves using AES-256-GCM with an Argon2id-derived password key
- Edit menu with Undo, Redo, Cut, Copy, Paste as Plain Text, and Select All
- Tools menu with a password generator that inserts at the cursor and copies to clipboard
- Multiline editing
- Word-wrap mode using `ImGuiInputTextFlags_WordWrap`
- Black editor background with a line-number gutter
- Cached document line index for faster status and gutter updates
- Saves UI settings to `editor_config.ini`
- ProggyClean font if `assets/fonts/ProggyClean.ttf` is present

Build:

```powershell
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
```

Run:

```powershell
.\build\imgui_text_editor.exe
```

The editor uses a small document model with `ImGui::InputTextMultiline`, `ImGuiInputTextFlags_WordWrap`, `ImGuiInputTextFlags_CallbackAlways`, `ImGuiInputTextFlags_CallbackResize`, `ImGuiInputTextFlags_AllowTabInput`, `ImGuiInputTextFlags_NoUndoRedo`, and `ImGuiInputTextFlags_NoHorizontalScroll`. Undo/redo history is tracked by the editor so menu commands and keyboard shortcuts share one history path.

Regular Save and Save As write plaintext for plaintext documents. Use Save Encrypted or Save Encrypted As to write an encrypted file. Once an encrypted file is opened, regular Save/Ctrl+S preserves encrypted mode and prompts for the password again. The encrypted file format stores an editor-specific header, Argon2id parameters and salt, an AES-GCM nonce, an authentication tag, and ciphertext. Opening one of these files prompts for the password.

On shutdown, the editor writes `editor_config.ini` with window size, word wrap, line-number visibility, and password generator settings. The file is loaded on startup.
