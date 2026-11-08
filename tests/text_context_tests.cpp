#include "text_context.hpp"
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using Microsoft::WRL::ComPtr;
struct Provider {
    std::wstring text;
    HRESULT read_status = S_OK;
    bool logical_eof = false;
    int readonly_end = 0;
};
// Подмена только внешней COM-границы. Character-перемещение намеренно может
// выйти за редактор, как у реально наблюдавшегося UIA-провайдера.
class Range final : public IUIAutomationTextRange {
public:
    Range(std::shared_ptr<Provider> provider, int start, int end, bool document = false)
        : provider_(std::move(provider)), start_(start), end_(end), document_(document) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if(!value) return E_POINTER;
        *value = nullptr;
        if(iid != IID_IUnknown && iid != IID_IUIAutomationTextRange) return E_NOINTERFACE;
        *value = static_cast<IUIAutomationTextRange*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --references_; if(left == 0) delete this; return left;
    }
    HRESULT STDMETHODCALLTYPE Clone(IUIAutomationTextRange** value) override {
        if(!value) return E_POINTER;
        auto* clone = new Range(provider_, start_, end_, document_);
        clone->normalized_end_ = normalized_end_;
        *value = clone; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Compare(IUIAutomationTextRange*, BOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CompareEndpoints(TextPatternRangeEndpoint from,
        IUIAutomationTextRange* other, TextPatternRangeEndpoint to, int* result) override {
        auto* range = dynamic_cast<Range*>(other);
        if(!range || !result || provider_ != range->provider_) return E_INVALIDARG;
        *result = Position(from) - range->Position(to);
        if(provider_->logical_eof && !document_ && !normalized_end_ && range->document_ && *result == 0 &&
            from == TextPatternRangeEndpoint_End && to == TextPatternRangeEndpoint_End) *result = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE MoveEndpointByRange(TextPatternRangeEndpoint from,
        IUIAutomationTextRange* other, TextPatternRangeEndpoint to) override {
        auto* range = dynamic_cast<Range*>(other);
        if(!range || provider_ != range->provider_) return E_INVALIDARG;
        if(from == TextPatternRangeEndpoint_End && to == TextPatternRangeEndpoint_End && range->document_) {
            normalized_end_ = true;
        }
        Set(from, range->Position(to)); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE MoveEndpointByUnit(TextPatternRangeEndpoint endpoint,
        TextUnit, int count, int* moved) override {
        if(!moved) return E_POINTER;
        const int previous = Position(endpoint);
        const int next = std::clamp(previous + count, 0, static_cast<int>(provider_->text.size()));
        Set(endpoint, next); *moved = next - previous; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetText(int maximum, BSTR* text) override {
        if(!text) return E_POINTER;
        *text = nullptr;
        if(FAILED(provider_->read_status)) return provider_->read_status;
        const int count = maximum < 0 ? end_ - start_ : std::min(maximum, end_ - start_);
        *text = SysAllocStringLen(provider_->text.data() + start_, static_cast<UINT>(count));
        return *text ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE ExpandToEnclosingUnit(TextUnit) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FindAttribute(TEXTATTRIBUTEID, VARIANT, BOOL, IUIAutomationTextRange**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FindText(BSTR, BOOL, BOOL, IUIAutomationTextRange**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetAttributeValue(TEXTATTRIBUTEID attribute, VARIANT* value) override {
        if(attribute != UIA_IsReadOnlyAttributeId || !value) return E_INVALIDARG;
        VariantInit(value);
        if(start_ < provider_->readonly_end && end_ > provider_->readonly_end) return S_OK;
        value->vt = VT_BOOL;
        value->boolVal = start_ < provider_->readonly_end ? VARIANT_TRUE : VARIANT_FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBoundingRectangles(SAFEARRAY**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetEnclosingElement(IUIAutomationElement**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Move(TextUnit, int, int*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Select() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AddToSelection() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ScrollIntoView(BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetChildren(IUIAutomationElementArray**) override { return E_NOTIMPL; }
private:
    [[nodiscard]] int Position(TextPatternRangeEndpoint endpoint) const {
        return endpoint == TextPatternRangeEndpoint_Start ? start_ : end_;
    }
    void Set(TextPatternRangeEndpoint endpoint, int position) {
        if(endpoint == TextPatternRangeEndpoint_Start) { start_ = position; end_ = std::max(end_, start_); }
        else { end_ = position; start_ = std::min(start_, end_); }
    }
    std::atomic<ULONG> references_{1};
    std::shared_ptr<Provider> provider_;
    int start_;
    int end_;
    bool document_;
    bool normalized_end_ = false;
};
void Require(bool value, const char* message) {
    if(!value) throw std::runtime_error(message);
}
struct Fixture {
    std::shared_ptr<Provider> provider = std::make_shared<Provider>();
    ComPtr<IUIAutomationTextRange> document, selection;
    Fixture(std::wstring_view field, int caret) {
        provider->text = std::wstring(7000, L'X') + std::wstring(field) + L"OUTSIDE";
        document.Attach(new Range(provider, 7000, 7000 + static_cast<int>(field.size()), true));
        selection.Attach(new Range(provider, 7000 + caret, 7000 + caret));
    }
};
}
int main() {
    try {
        lshift::AutomationContext context;
        Fixture word(L"ghbdtn  ", 8);
        Require(lshift::ReadAutomationContext(word.document.Get(), word.selection.Get(), context) &&
            context.prefix == L"ghbdtn  " && context.following == L'\0',
            "word and spaces stay inside the editor even when character moves escape it");
        std::cout << "PASS editor bounds exclude surrounding document\n";
        word.provider->logical_eof = true;
        Require(lshift::ReadAutomationContext(word.document.Get(), word.selection.Get(), context) &&
            context.prefix == L"ghbdtn  " && context.following == L'\0',
            "logical EOF is accepted only when the whole prefix equals the whole editor text");
        Fixture outside_end(L"ghbdtn", 7);
        Require(!lshift::ReadAutomationContext(outside_end.document.Get(), outside_end.selection.Get(), context) &&
            context.status == E_BOUNDS, "text beyond the editor is not accepted as a logical EOF");
        std::cout << "PASS logical EOF and rejected outside text\n";
        Fixture middle(L"ghbdtn) tail", 6);
        Require(lshift::ReadAutomationContext(middle.document.Get(), middle.selection.Get(), context) &&
            context.prefix == L"ghbdtn" && context.following == L')', "following character belongs to this editor");
        Fixture beginning(L"ghbdtn", 0);
        Require(lshift::ReadAutomationContext(beginning.document.Get(), beginning.selection.Get(), context) &&
            context.prefix.empty() && context.following == L'g', "start of editor does not read preceding window text");
        Fixture empty(L"", 0);
        Require(lshift::ReadAutomationContext(empty.document.Get(), empty.selection.Get(), context) &&
            context.prefix.empty() && context.following == L'\0', "empty editor remains empty");
        std::cout << "PASS middle, beginning and empty editor\n";
        Fixture long_field(std::wstring(5000, L'a') + L" ghbdtn  ", 5009);
        Require(lshift::ReadAutomationContext(long_field.document.Get(), long_field.selection.Get(), context) &&
            context.prefix.size() == 4096 && context.prefix.ends_with(L" ghbdtn  ") && !context.prefix_complete,
            "long bounded prefix retains recent word instead of the beginning of the document");
        Fixture oversized(std::wstring(65537, L'a'), 65537);
        Require(!lshift::ReadAutomationContext(oversized.document.Get(), oversized.selection.Get(), context) &&
            FAILED(context.status), "oversized provider text cannot be used as a partial prefix");
        Fixture stale(L"ghbdtn", -1);
        Require(!lshift::ReadAutomationContext(stale.document.Get(), stale.selection.Get(), context) &&
            context.stage == lshift::ContextStage::Bounds, "caret outside editor is rejected");
        word.provider->read_status = E_ACCESSDENIED;
        Require(!lshift::ReadAutomationContext(word.document.Get(), word.selection.Get(), context) &&
            context.stage == lshift::ContextStage::PrefixRead && context.status == E_ACCESSDENIED,
            "provider error retains exact stage and HRESULT");
        std::cout << "PASS bounded long text and provider failures\n";
        Fixture mixed(L"quote ghbdtn  ", 14);
        mixed.provider->readonly_end = 7006;
        Require(lshift::WritableAutomationTarget(mixed.document.Get(), mixed.selection.Get(), L"ghbdtn  "),
            "a writable word and its trailing spaces can be corrected in a mixed editor");
        mixed.provider->logical_eof = true;
        Require(lshift::ReadAutomationContext(mixed.document.Get(), mixed.selection.Get(), context) &&
            lshift::WritableAutomationTarget(mixed.document.Get(), mixed.selection.Get(), L"ghbdtn  ", context.logical_eof),
            "a writable word at the verified logical end of the editor can be corrected");
        Require(!lshift::WritableAutomationTarget(mixed.document.Get(), mixed.selection.Get(), L"wrong   ", context.logical_eof),
            "provider range text must match the actual text to be removed");
        Require(!lshift::WritableAutomationTarget(mixed.document.Get(), mixed.selection.Get(), L" ghbdtn  ", context.logical_eof),
            "the text to be removed cannot include a protected character");
        Require(!lshift::WritableAutomationTarget(mixed.document.Get(), mixed.selection.Get(), L"Xquote ghbdtn  ", context.logical_eof),
            "the word range cannot cross the editor boundary even if the text matches");

        ComPtr<IUIAutomationTextRange> selected_word, selected_across_quote, selected_quote;
        selected_word.Attach(new Range(mixed.provider, 7006, 7012));
        selected_across_quote.Attach(new Range(mixed.provider, 7000, 7012));
        selected_quote.Attach(new Range(mixed.provider, 7000, 7005));
        Require(lshift::WritableAutomationTarget(mixed.document.Get(), selected_word.Get()),
            "writable selection can be replaced in a mixed editor");
        Require(!lshift::WritableAutomationTarget(mixed.document.Get(), selected_across_quote.Get()) &&
            !lshift::WritableAutomationTarget(mixed.document.Get(), selected_quote.Get()),
            "mixed and protected selections cannot be replaced");
        std::cout << "PASS edit targets stay within writable editor text\n";
        Fixture codex(L"Nfr ;t", 6);
        codex.provider->logical_eof = true;
        ComPtr<IUIAutomationTextRange> selected_all;
        selected_all.Attach(new Range(codex.provider, 7000, 7006));
        Require(lshift::WritableAutomationTarget(codex.document.Get(), selected_all.Get()),
            "selecting all text accepts a logical EOF beyond the final text endpoint");
        ComPtr<IUIAutomationTextRange> selected_outside;
        selected_outside.Attach(new Range(codex.provider, 7000, 7007));
        Require(!lshift::WritableAutomationTarget(codex.document.Get(), selected_outside.Get()),
            "actual text outside the editor cannot be treated as a logical EOF");
        codex.provider->readonly_end = 7006;
        Require(!lshift::WritableAutomationTarget(codex.document.Get(), selected_all.Get()),
            "normalizing a logical EOF does not permit replacement of protected text");
        std::cout << "PASS selected logical EOF preserves text and write boundaries\n";
    } catch(const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
