#include "filter/filter_pipeline.h"
#include "filter/job_identity.h"
#include "bridge/metadata_merger.h"
#include "storage/file_utils.h"
#include "../tools/xps_pipeline_mocks.h"
#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <fstream>

namespace
{
using namespace wpce::test_support;
using wpce::common::com_ptr;

template<class Interface>
class Part : public Interface
{
 public:
  virtual ~Part() = default;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override
  {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (id != IID_IUnknown && id != __uuidof(Interface)) return E_NOINTERFACE;
    *out = static_cast<Interface*>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
  ULONG STDMETHODCALLTYPE Release() override { const auto n = InterlockedDecrement(&refs); if (!n) delete this; return n; }
  HRESULT STDMETHODCALLTYPE GetUri(BSTR* out) override { *out = nullptr; return S_FALSE; }
  HRESULT STDMETHODCALLTYPE GetPrintTicket(IPartPrintTicket** out) override { *out = nullptr; return S_FALSE; }
  HRESULT STDMETHODCALLTYPE SetPrintTicket(IPartPrintTicket*) override { return E_NOTIMPL; }
  LONG refs{1};
};
class Page final : public Part<IFixedPage>
{
 public:
  bool provide_stream{false};
  HRESULT STDMETHODCALLTYPE GetStream(IPrintReadStream** out) override;
  HRESULT STDMETHODCALLTYPE GetPartCompression(EXpsCompressionOptions*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPartCompression(EXpsCompressionOptions) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetPagePart(const wchar_t*, IUnknown**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetWriteStream(IPrintWriteStream**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPagePart(IUnknown*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE DeleteResource(const wchar_t*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetXpsPartIterator(IXpsPartIterator**) override { return E_NOTIMPL; }
};
class Stream final : public IPrintReadStream
{
 public:
  Stream() { ++live; }
  ~Stream() { --live; }
  inline static std::atomic_int live{0};
  bool throw_read{false};
  std::string content;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override
  { *out = nullptr; if (id != IID_IUnknown && id != __uuidof(IPrintReadStream)) return E_NOINTERFACE; *out=this; AddRef(); return S_OK; }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
  ULONG STDMETHODCALLTYPE Release() override { auto n=InterlockedDecrement(&refs); if (!n) delete this; return n; }
  HRESULT STDMETHODCALLTYPE Seek(LONGLONG offset, DWORD, ULONGLONG* position) override
  { if(offset) return E_NOTIMPL; if(position) *position=0; return S_OK; }
  HRESULT STDMETHODCALLTYPE ReadBytes(void* dest, ULONG size, ULONG* read, BOOL* eof) override
  {
    if (throw_read) throw std::bad_alloc{};
    const std::string xml = content.empty() ? "<psf:PrintTicket xmlns:psf=\"http://schemas.microsoft.com/windows/2003/08/printing/printschemaframework\"/>" : content;
    *read = static_cast<ULONG>(std::min<std::size_t>(size, xml.size()));
    memcpy(dest, xml.data(), *read); *eof = TRUE; return S_OK;
  }
 private: LONG refs{1};
};
HRESULT Page::GetStream(IPrintReadStream** out)
{
  if (!provide_stream) { *out=nullptr; return S_FALSE; }
  auto* stream=new Stream; stream->content="<FixedPage/>"; *out=stream; return S_OK;
}
class Ticket final : public IPartPrintTicket
{
 public:
  bool throw_read{false};
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override
  { *out=nullptr; if(id!=IID_IUnknown && id!=__uuidof(IPartPrintTicket)) return E_NOINTERFACE; *out=this; AddRef(); return S_OK; }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
  ULONG STDMETHODCALLTYPE Release() override { auto n=InterlockedDecrement(&refs); if(!n) delete this; return n; }
  HRESULT STDMETHODCALLTYPE GetUri(BSTR* out) override { *out=nullptr; return S_FALSE; }
  HRESULT STDMETHODCALLTYPE GetStream(IPrintReadStream** out) override
  { auto* stream=new Stream; stream->throw_read=throw_read; *out=stream; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetPartCompression(EXpsCompressionOptions*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPartCompression(EXpsCompressionOptions) override { return E_NOTIMPL; }
 private: LONG refs{1};
};

class FilterSafety : public ::testing::Test
{
 protected:
  com_ptr<MockFixedDocumentSequence> sequence{new MockFixedDocumentSequence};
  com_ptr<Part<IFixedDocument>> document{new Part<IFixedDocument>};
  com_ptr<Page> page{new Page};
  com_ptr<Ticket> ticket{new Ticket};
  com_ptr<MockXpsProvider> provider{new MockXpsProvider({sequence.get(), document.get(), page.get()})};
  com_ptr<MockXpsConsumer> consumer{new MockXpsConsumer};
  com_ptr<MockCommunicator> communicator{new MockCommunicator(provider.get(), consumer.get())};
  com_ptr<MockPipelineControl> control{new MockPipelineControl};
  com_ptr<MockPropertyBag> bag{new MockPropertyBag};
  com_ptr<wpce::filter::XpsCaptureFilter> filter{new wpce::filter::XpsCaptureFilter};
  std::filesystem::path root;
  void SetUp() override
  {
    root=std::filesystem::temp_directory_path()/ ("wpce-filter-safety-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    filter->set_output_directory(root);
    sequence->ticket=ticket.get();
  }
  void TearDown() override { std::error_code ec; std::filesystem::remove_all(root,ec); }
  HRESULT run() { EXPECT_EQ(filter->InitializeFilter(communicator.get(),bag.get(),control.get()),S_OK); return filter->StartOperation(); }
};

TEST_F(FilterSafety, StandardUnsignedIdentityPersistsAndAllPartsForwardInOrder)
{
  EXPECT_EQ(run(),S_OK);
  EXPECT_EQ(consumer->forwarded,(std::vector<IUnknown*>{sequence.get(),document.get(),page.get()}));
  ASSERT_EQ(filter->page_artifacts().size(),1u); EXPECT_EQ(filter->page_artifacts()[0].job_id,42u);
  wpce::core::MetadataSnapshot metadata;
  ASSERT_TRUE(wpce::bridge::load_pt_metadata(root/(filter->page_artifacts()[0].capture_id+".pt-message.json"),metadata));
  EXPECT_EQ(metadata.job_id,42u); EXPECT_EQ(metadata.printer_name,"Mock Test Printer");
  EXPECT_EQ(consumer->close_count,1); EXPECT_EQ(control->finish_count,1);
  filter->ShutdownOperation(); filter->ShutdownOperation(); EXPECT_EQ(control->finish_count,1);
  EXPECT_EQ(sequence->AddRef(),2u); EXPECT_EQ(sequence->Release(),1u); // only fixture reference remains
}
TEST_F(FilterSafety, IdentityBoundaryAndSignedCompatibility)
{
  bag->job_id=MAXDWORD; EXPECT_EQ(wpce::filter::read_job_identity(bag.get()),MAXDWORD);
  bag->id_type=VT_I4; bag->job_id=42; EXPECT_EQ(wpce::filter::read_job_identity(bag.get()),42u);
  bag->job_id=MAXDWORD; EXPECT_FALSE(wpce::filter::read_job_identity(bag.get()));
  bag->job_id=0; EXPECT_FALSE(wpce::filter::read_job_identity(bag.get()));
  bag->id_type=VT_EMPTY; EXPECT_FALSE(wpce::filter::read_job_identity(bag.get()));
  bag->id_result=E_FAIL; EXPECT_FALSE(wpce::filter::read_job_identity(bag.get()));
  EXPECT_FALSE(wpce::filter::read_job_identity(nullptr));
}
TEST_F(FilterSafety, MaximumUnsignedIdentitySurvivesMetadataBridge)
{
  bag->job_id=MAXDWORD; EXPECT_EQ(run(),S_OK);
  wpce::core::MetadataSnapshot metadata;
  ASSERT_TRUE(wpce::bridge::load_pt_metadata(root/(filter->page_artifacts()[0].capture_id+".pt-message.json"),metadata));
  EXPECT_EQ(metadata.job_id,MAXDWORD);
  ASSERT_EQ(filter->page_artifacts().size(),1u); EXPECT_EQ(filter->page_artifacts()[0].job_id,MAXDWORD);
}
TEST_F(FilterSafety, FixedPageAndEveryNonemptyReferenceExistWithExactContents)
{
  page->provide_stream=true; EXPECT_EQ(run(),S_OK);
  ASSERT_EQ(filter->page_artifacts().size(),1u); const auto& index=filter->page_artifacts()[0];
  EXPECT_EQ(index.fixed_page_state,"reported"); EXPECT_FALSE(index.fixed_page.empty());
  for(const auto& name:{index.job_print_ticket,index.document_print_ticket,index.page_print_ticket,index.fixed_page})
    if(!name.empty()) EXPECT_TRUE(std::filesystem::is_regular_file(root/"pages"/name));
  std::ifstream input(root/"pages"/index.fixed_page); std::string content{std::istreambuf_iterator<char>(input),{}};
  EXPECT_EQ(content,"<FixedPage/>"); EXPECT_EQ(consumer->forwarded_count_,3u);
}
TEST_F(FilterSafety, MissingPageStreamHasNoInventedFileReference)
{
  EXPECT_EQ(run(),S_OK); ASSERT_EQ(filter->page_artifacts().size(),1u);
  const auto& index=filter->page_artifacts()[0];
  EXPECT_EQ(index.fixed_page_state,"unsupported"); EXPECT_TRUE(index.fixed_page.empty());
  EXPECT_TRUE(index.document_print_ticket.empty()); EXPECT_TRUE(index.page_print_ticket.empty());
}
TEST_F(FilterSafety, InvalidIdentityDoesNotPublishZeroButStillPrints)
{
  bag->job_id=0; EXPECT_EQ(run(),S_OK); EXPECT_EQ(consumer->forwarded_count_,3u);
  EXPECT_TRUE(filter->page_artifacts().empty()); EXPECT_FALSE(std::filesystem::exists(root));
  EXPECT_FALSE(filter->last_error().empty());
}
TEST_F(FilterSafety, CaptureAllocationFailureStillForwardsOriginalParts)
{
  sequence->throw_capture=true; EXPECT_EQ(run(),S_OK); EXPECT_EQ(consumer->forwarded_count_,3u);
  EXPECT_EQ(consumer->forwarded.front(),sequence.get()); EXPECT_EQ(control->finish_count,1);
}
TEST_F(FilterSafety, CaptureStreamExceptionReleasesEveryAcquiredReference)
{
  ticket->throw_read=true;
  EXPECT_EQ(run(),S_OK); EXPECT_EQ(consumer->forwarded_count_,3u);
  EXPECT_EQ(Stream::live.load(),0); EXPECT_EQ(control->finish_count,1);
}
TEST_F(FilterSafety, ProviderFailurePreservesFirstErrorAndClosesOnce)
{
  provider->fail_at=1; provider->read_failure=E_ABORT; consumer->close_result=E_FAIL;
  EXPECT_EQ(run(),E_ABORT); EXPECT_EQ(consumer->forwarded_count_,1u);
  EXPECT_EQ(consumer->close_count,1); EXPECT_EQ(control->finish_count,1);
}
TEST_F(FilterSafety, ConsumerFailureStopsFurtherSending)
{
  consumer->fail_at=1; EXPECT_EQ(run(),E_ACCESSDENIED);
  EXPECT_EQ(consumer->forwarded_count_,1u); EXPECT_EQ(consumer->close_count,1); EXPECT_EQ(control->finish_count,1);
}
TEST_F(FilterSafety, CloseFailureIsNotSuccess)
{
  consumer->close_result=E_FAIL; EXPECT_EQ(run(),E_FAIL); EXPECT_EQ(control->finish_count,1);
}
TEST_F(FilterSafety, SuccessfulNullPartIsNormalEndOfStream)
{
  provider->terminal_result=S_OK; EXPECT_EQ(run(),S_OK); EXPECT_EQ(control->finish_count,1);
}
TEST_F(FilterSafety, CancelDuringReadKeepsReferencesUntilSingleCompletion)
{
  provider->before_read=[this] { std::thread cancel([this] { filter->ShutdownOperation(); }); cancel.join(); };
  EXPECT_EQ(run(),HRESULT_FROM_WIN32(ERROR_CANCELLED));
  EXPECT_EQ(consumer->forwarded_count_,0u); EXPECT_EQ(consumer->close_count,1); EXPECT_EQ(control->finish_count,1);
}
TEST_F(FilterSafety, CancelBeforeStartFinishesOnlyOnce)
{
  ASSERT_EQ(filter->InitializeFilter(communicator.get(),bag.get(),control.get()),S_OK);
  filter->ShutdownOperation(); EXPECT_EQ(filter->StartOperation(),HRESULT_FROM_WIN32(ERROR_CANCELLED));
  filter->ShutdownOperation(); EXPECT_EQ(control->finish_count,1);
}
TEST_F(FilterSafety, ReaderNegotiationFailureStillFinishes)
{
  com_ptr<MockCommunicator> broken{new MockCommunicator(nullptr,consumer.get())};
  ASSERT_EQ(filter->InitializeFilter(broken.get(),bag.get(),control.get()),S_OK);
  EXPECT_EQ(filter->StartOperation(),E_FAIL); EXPECT_EQ(control->finish_count,1);
}
TEST_F(FilterSafety, CaptureDiskFailureDoesNotFailPrintThrough)
{
  std::filesystem::create_directories(root.parent_path());
  wpce::storage::write_text_file_atomic("blocks a directory",root);
  EXPECT_EQ(run(),S_OK); EXPECT_EQ(consumer->forwarded_count_,3u); EXPECT_EQ(control->finish_count,1);
}
}
