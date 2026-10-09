// In-memory dialog template (no .rc dialog resource needed), shared by the Pro and welcome dialogs.
#pragma once

#include "common.h"

namespace ui {

// Predefined control classes for Template::Item.
constexpr WORD kButton = 0x0080, kEdit = 0x0081, kStatic = 0x0082;

class Template {
public:
    void Dialog(const wchar_t* title, short cx, short cy, WORD items) {
        Dword(DS_SETFONT | DS_MODALFRAME | DS_CENTER | DS_SHELLFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU);
        Dword(0);
        Word(items);
        Word(0), Word(0), Word(WORD(cx)), Word(WORD(cy));
        Word(0), Word(0);  // no menu, default class
        Str(title);
        Word(9);
        Str(L"Segoe UI");
    }
    void Item(WORD cls, const wchar_t* text, int id, short x, short y, short cx, short cy, DWORD style) {
        while (buf_.size() % 4) buf_.push_back(0);
        Dword(style | WS_CHILD | WS_VISIBLE);
        Dword(0);
        Word(WORD(x)), Word(WORD(y)), Word(WORD(cx)), Word(WORD(cy));
        Word(WORD(id));
        Word(0xFFFF), Word(cls);
        Str(text);
        Word(0);
    }
    const DLGTEMPLATE* Get() const { return reinterpret_cast<const DLGTEMPLATE*>(buf_.data()); }

private:
    void Word(WORD v) { buf_.insert(buf_.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 2); }
    void Dword(DWORD v) { Word(LOWORD(v)), Word(HIWORD(v)); }
    void Str(const wchar_t* s) {
        do Word(*s);
        while (*s++);
    }
    std::vector<uint8_t> buf_;
};

}  // namespace ui
