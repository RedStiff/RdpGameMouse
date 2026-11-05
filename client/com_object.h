// Мінімальна реалізація IUnknown для callback-об'єктів плагіна.
#pragma once
#include <windows.h>

template <class I>
class ComObject : public I {
    LONG ref_ = 1;

public:
    virtual ~ComObject() {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(I)) {
            *ppv = static_cast<I*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }
};
