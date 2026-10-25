#include "storage/reliable_file_writer.h"
#include "storage/job_snapshot_writer.h"
#include "storage/page_artifact_writer.h"
#include "bridge/print_ticket_inbox.h"
#include "common/physical_job_identity.h"
#include "filter/capture_identity.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>
#include <set>
#include <psapi.h>

namespace
{
namespace fs = std::filesystem;
using namespace wpce;
std::string read(const fs::path& file) { std::ifstream in(file,std::ios::binary); return {std::istreambuf_iterator<char>(in),{}}; }
template<class Predicate> bool wait_for(Predicate predicate)
{
  const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(!predicate() && std::chrono::steady_clock::now()<end) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  return predicate();
}
class CaptureReliability : public testing::Test
{
 protected:
  fs::path root;
  void SetUp() override { root=fs::temp_directory_path()/ ("wpce-reliability-"+storage::unique_storage_id()); fs::create_directories(root); }
  void TearDown() override { std::error_code error; fs::remove_all(root,error); }
  bridge::PrintTicketMessage message(std::string id="message-1", std::string physical="physical-1")
  {
    bridge::PrintTicketMessage result; result.message_id=std::move(id); result.physical_identity=std::move(physical);
    result.metadata.job_id=42; result.metadata.printer_name="Printer"; result.metadata.copies={"2",core::FieldState::Reported}; return result;
  }
  void put(const std::string& name, const bridge::PrintTicketMessage& value)
  { storage::write_text_file_atomic(bridge::serialize_pt_message(value),root/(name+".pt-message.json")); }
};
TEST_F(CaptureReliability, PrinterIdentitiesAreStableBoundedAndDoNotCollide)
{
  EXPECT_EQ(storage::stable_digest("abc"),"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const std::vector<std::string> printers={"HP A","HP_A","hp a", "HP/A", "HP\\A", "", "CON", "打印机甲", "打印机乙",std::string(1000,'x')};
  std::set<std::string> ids;
  for(const auto& name:printers)
  {
    const auto id=storage::printer_storage_token(name); EXPECT_EQ(id.size(),66u);
    EXPECT_TRUE(storage::valid_artifact_filename(id)); EXPECT_TRUE(ids.insert(id).second);
    EXPECT_EQ(storage::printer_storage_token(name),id);
  }
}
TEST_F(CaptureReliability, UnicodeCaptureRootPersistsAndInboxEnvironmentKeepsNativePath)
{
  const auto directory=root/u8"打印数据🖨";
  storage::ReliableFileWriter writer({.root=directory,.queue_name="unicode"}); writer.start();
  ASSERT_TRUE(writer.enqueue({{"value.txt","UTF-8 content"}})); writer.stop();
  EXPECT_TRUE(writer.drained()); EXPECT_EQ(read(directory/"value.txt"),"UTF-8 content");
  const auto needed=GetEnvironmentVariableW(L"WPCE_OUTPUT_DIR",nullptr,0);
  std::wstring previous(needed,L'\0'); if(needed) GetEnvironmentVariableW(L"WPCE_OUTPUT_DIR",previous.data(),needed);
  struct Restore { std::wstring value; ~Restore() { SetEnvironmentVariableW(L"WPCE_OUTPUT_DIR",value.empty()?nullptr:value.c_str()); } } restore{previous};
  ASSERT_TRUE(SetEnvironmentVariableW(L"WPCE_OUTPUT_DIR",directory.c_str()));
  const auto candidates=bridge::pt_inbox_directories(root);
  EXPECT_NE(std::find(candidates.begin(),candidates.end(),directory),candidates.end());
}
TEST_F(CaptureReliability, UnsafeArtifactNamesAreRejectedWithoutTraversal)
{
  for(const auto* name:{"../x","a/b","a\\b","CON","NUL.xml","LPT1.txt","x.","x ","a:b"}) EXPECT_FALSE(storage::valid_artifact_filename(name));
  storage::ReliableFileWriter writer({.root=root,.queue_name="test"}); writer.start();
  EXPECT_FALSE(writer.enqueue({{"../escape.txt","bad"}})); writer.stop();
  EXPECT_FALSE(fs::exists(root.parent_path()/"escape.txt"));
}
TEST_F(CaptureReliability, CaptureJournalCannotOverwriteOrderFilesInAnyCase)
{
  storage::write_text_file_atomic("protected",root/"orders.json");
  storage::ReliableFileWriter writer({.root=root,.queue_name="test"}); writer.start();
  EXPECT_FALSE(writer.enqueue({{"ORDERS.JSON","bad"}}));
  EXPECT_FALSE(writer.enqueue({{"_STATE/counters.json","bad"}})); writer.stop();
  EXPECT_EQ(read(root/"orders.json"),"protected");
}
TEST_F(CaptureReliability, TransientWorkerFailureRetriesWithoutLosingAcceptedItem)
{
  std::atomic_bool fail{true};
  storage::ReliableFileWriter writer({.root=root,.queue_name="retry",.worker_hook=[&](auto stage,const auto&) {
    if(fail && stage==storage::AtomicWriteStage::Replace) throw std::runtime_error("temporary disk lock");
  }});
  writer.start(); ASSERT_TRUE(writer.enqueue({{"value.txt","accepted"}}));
  EXPECT_TRUE(wait_for([&] { return !writer.last_error().empty(); })); EXPECT_EQ(writer.pending_count(),1u);
  fail=false; EXPECT_TRUE(wait_for([&] { return writer.drained(); }));
  writer.stop(); EXPECT_TRUE(writer.drained()); EXPECT_EQ(read(root/"value.txt"),"accepted");
}
TEST_F(CaptureReliability, PersistentFailureStopsWithDurablePendingAndFreshInstanceReplays)
{
  {
    storage::ReliableFileWriter writer({.root=root,.queue_name="replay",.worker_hook=[](auto,const auto&) { throw std::runtime_error("offline"); }});
    writer.start(); ASSERT_TRUE(writer.enqueue({{"saved.txt","retained"}})); writer.stop();
    EXPECT_FALSE(writer.drained()); EXPECT_EQ(writer.pending_count(),1u); EXPECT_GT(writer.pending_bytes(),0u);
  }
  storage::ReliableFileWriter recovered({.root=root,.queue_name="replay"}); recovered.start(); recovered.stop();
  EXPECT_TRUE(recovered.drained()); EXPECT_EQ(read(root/"saved.txt"),"retained");
}
TEST_F(CaptureReliability, AcceptedTransactionsKeepOrderAcrossRestart)
{
  {
    storage::ReliableFileWriter writer({.root=root,.queue_name="order",.worker_hook=[](auto,const auto&) { throw std::runtime_error("offline"); }});
    writer.start();
    for(int i=0;i<20;++i) ASSERT_TRUE(writer.enqueue({{"last.txt",std::to_string(i)}}));
    writer.stop(); EXPECT_EQ(writer.pending_count(),20u);
  }
  storage::ReliableFileWriter recovered({.root=root,.queue_name="order"}); recovered.start(); recovered.stop();
  EXPECT_EQ(read(root/"last.txt"),"19"); EXPECT_TRUE(recovered.drained());
}
TEST_F(CaptureReliability, SharedWritersStopAtTheirAcceptedBoundaryWithoutFalseFailure)
{
  storage::ReliableFileWriter first({.root=root,.queue_name="shared"}),second({.root=root,.queue_name="shared"});
  first.start(); second.start();
  std::atomic_bool accepted{true};
  auto submit=[&](auto& writer,const std::string& prefix) {
    for(int i=0;i<16;++i) if(!writer.enqueue({{prefix+std::to_string(i)+".txt",prefix}})) accepted=false;
  };
  std::thread a([&] {submit(first,"a");}),b([&] {submit(second,"b");}); a.join(); b.join();
  first.stop(); second.stop(); EXPECT_TRUE(accepted); EXPECT_TRUE(first.drained()); EXPECT_TRUE(second.drained());
  for(int i=0;i<16;++i) { EXPECT_EQ(read(root/("a"+std::to_string(i)+".txt")),"a"); EXPECT_EQ(read(root/("b"+std::to_string(i)+".txt")),"b"); }
}
TEST_F(CaptureReliability, StopRequestLeavesBusyWorkerAliveUntilIoCompletes)
{
  std::atomic_bool entered{false}, release{false};
  storage::ReliableFileWriter writer({.root=root,.queue_name="busy",.worker_hook=[&](auto stage,const auto&) {
    if(stage==storage::AtomicWriteStage::Replace) {entered=true; while(!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));}
  }});
  writer.start(); ASSERT_TRUE(writer.enqueue({{"result.txt","preserved"}}));
  EXPECT_TRUE(wait_for([&] {return entered.load();})); writer.request_stop(); EXPECT_TRUE(writer.running());
  release=true; EXPECT_TRUE(wait_for([&] {return !writer.running();})); writer.stop();
  EXPECT_TRUE(writer.drained()); EXPECT_EQ(read(root/"result.txt"),"preserved");
}
TEST_F(CaptureReliability, LargeBacklogRemainsOnDiskAndNewSubmissionHasExplicitResult)
{
  storage::ReliableFileWriter writer({.root=root,.queue_name="backlog",.max_item_bytes=2048,
      .worker_hook=[](auto,const auto&) { throw std::runtime_error("slow disk"); }});
  writer.start();
  for(int i=0;i<100;++i) ASSERT_TRUE(writer.enqueue({{"item-"+std::to_string(i)+".txt",std::string(256,'x')}}));
  EXPECT_FALSE(writer.enqueue({{"oversized.txt",std::string(3000,'x')}}));
  writer.stop(); EXPECT_EQ(writer.pending_count(),100u); EXPECT_GT(writer.pending_bytes(),25600u);
  EXPECT_FALSE(writer.enqueue({{"stopped.txt","rejected"}}));
}
TEST_F(CaptureReliability, BacklogPressureMeasuresMemoryAndJournalLatency)
{
  PROCESS_MEMORY_COUNTERS_EX before{}, after{};
  ASSERT_TRUE(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&before),sizeof(before)));
  const auto start=std::chrono::steady_clock::now(); long long max_microseconds=0;
  storage::ReliableFileWriter writer({.root=root,.queue_name="pressure",.worker_hook=[](auto,const auto&) {throw std::runtime_error("slow disk");}});
  writer.start();
  for(int i=0;i<256;++i)
  {
    const auto begin=std::chrono::steady_clock::now();
    ASSERT_TRUE(writer.enqueue({{"item-"+std::to_string(i)+".txt",std::string(128*1024,'a')}}));
    max_microseconds=std::max(max_microseconds,std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-begin).count());
  }
  writer.stop();
  ASSERT_TRUE(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&after),sizeof(after)));
  const auto growth=after.PrivateUsage>before.PrivateUsage ? after.PrivateUsage-before.PrivateUsage : 0;
  EXPECT_LT(growth,16u*1024*1024); EXPECT_EQ(writer.pending_count(),256u);
  RecordProperty("private_memory_growth_bytes",std::to_string(growth));
  RecordProperty("working_set_bytes",std::to_string(after.WorkingSetSize));
  RecordProperty("pending_bytes",std::to_string(writer.pending_bytes()));
  RecordProperty("max_journal_enqueue_microseconds",std::to_string(max_microseconds));
  RecordProperty("total_milliseconds",std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count()));
}
TEST_F(CaptureReliability, ConcurrentInboxConsumersDeliverOnce)
{
  put("one",message()); std::atomic_int delivered{0};
  auto consume=[&] { bridge::consume_pt_inbox({root},[&](const auto&) { ++delivered; std::this_thread::sleep_for(std::chrono::milliseconds(20)); return true; }); };
  std::thread first(consume),second(consume); first.join(); second.join(); consume(); EXPECT_EQ(delivered,1);
}
TEST_F(CaptureReliability, DeliveryFailureRetainsClaimAndRetryAcknowledges)
{
  put("one",message());
  const auto pending=bridge::consume_pt_inbox({root},[](const auto&) { return false; });
  EXPECT_EQ(pending.pending,1u); EXPECT_TRUE(fs::exists(root/"one.pt-message.json.claim"));
  const auto retry=bridge::consume_pt_inbox({root},[](const auto&) { return true; });
  EXPECT_EQ(retry.delivered,1u); EXPECT_FALSE(fs::exists(root/"one.pt-message.json.claim"));
}
TEST_F(CaptureReliability, UnmatchedClaimsDoNotStarveLaterMessages)
{
  put("a",message("a","unmatched")); put("b",message("b","matched"));
  std::size_t delivered=0;
  for(int i=0;i<4;++i)
    delivered+=bridge::consume_pt_inbox({root},[](const auto& item) { return item.physical_identity=="matched"; },1).delivered;
  EXPECT_EQ(delivered,1u); EXPECT_TRUE(fs::exists(root/"a.pt-message.json.claim"));
}
TEST_F(CaptureReliability, ThrowingDeliveryDoesNotDeleteMessage)
{
  put("one",message()); const auto result=bridge::consume_pt_inbox({root},[](const auto&) -> bool { throw std::runtime_error("handler"); });
  EXPECT_EQ(result.pending,1u); EXPECT_FALSE(result.errors.empty()); EXPECT_TRUE(fs::exists(root/"one.pt-message.json.claim"));
}
TEST_F(CaptureReliability, PersistentDedupAndConflictingDuplicateAreDistinct)
{
  int count=0; auto callback=[&](const auto&) { ++count; return true; };
  const auto original=message(); put("first",original); bridge::consume_pt_inbox({root},callback);
  put("duplicate",original); bridge::consume_pt_inbox({root},callback); EXPECT_EQ(count,1);
  auto conflict=original; conflict.metadata.copies.value="99"; put("conflict",conflict);
  const auto result=bridge::consume_pt_inbox({root},callback); EXPECT_EQ(result.quarantined,1u); EXPECT_EQ(count,1);
}
TEST_F(CaptureReliability, MalformedAndLegacyMessagesArePreservedInQuarantine)
{
  storage::write_text_file_atomic("{broken",root/"broken.pt-message.json");
  auto noIdentity=message(); noIdentity.physical_identity.clear(); put("unidentified",noIdentity);
  storage::write_text_file_atomic("{\"legacy\":true}",root/"job-old-42-pt-metadata.json");
  const auto result=bridge::consume_pt_inbox({root},[](const auto&) { ADD_FAILURE(); return true; });
  EXPECT_EQ(result.quarantined,3u); EXPECT_EQ(std::distance(fs::directory_iterator(root/"_pt-quarantine"),fs::directory_iterator{}),3);
}
TEST_F(CaptureReliability, ReusedRawIdAndDifferentPrintersMatchOnlyExactPhysicalIdentity)
{
  core::JobSnapshot old; old.job_id=100; old.external_job_id=42; old.printer_name="Printer";
  old.user="alice"; old.document_name="file.pdf"; old.submit_time_utc="2026-10-24T01:00:00";
  auto newer=old; newer.job_id=101; newer.submit_time_utc="2026-10-24T02:00:00";
  auto other=newer; other.job_id=102; other.printer_name="Other";
  const std::unordered_map<std::uint32_t,core::JobSnapshot> jobs{{100,old},{101,newer},{102,other}};
  auto first=message("first",common::physical_job_identity(old)); auto second=message("second",common::physical_job_identity(newer));
  EXPECT_EQ(bridge::match_pt_job(first,jobs),100u); EXPECT_EQ(bridge::match_pt_job(second,jobs),101u);
  second.metadata.printer_name="Other"; EXPECT_EQ(bridge::match_pt_job(second,jobs),0u);
  second.physical_identity=common::physical_job_identity(other); EXPECT_EQ(bridge::match_pt_job(second,jobs),102u);
  put("one",first); put("two",message("two",common::physical_job_identity(newer)));
  EXPECT_EQ(bridge::consume_pt_inbox({root},[](const auto&) { return true; }).delivered,2u);
}
TEST_F(CaptureReliability, MetadataParsingSupportsUnicodeAndUnsignedIdentityTransactionally)
{
  auto value=message(); value.metadata.job_id=UINT32_MAX; value.metadata.printer_name="打印机甲";
  bridge::PrintTicketMessage parsed;
  EXPECT_TRUE(bridge::parse_pt_message(bridge::serialize_pt_message(value),parsed));
  EXPECT_EQ(parsed.metadata.job_id,UINT32_MAX); EXPECT_EQ(parsed.metadata.printer_name,value.metadata.printer_name);
  EXPECT_FALSE(bridge::parse_pt_message("{}",parsed)); EXPECT_EQ(parsed.metadata.job_id,UINT32_MAX);
}
TEST_F(CaptureReliability, PageReferencesUseExactNamesAndPublishOnlyAfterPayloads)
{
  storage::AsyncPageArtifactWriter writer({.output_directory=root}); writer.start();
  storage::PageArtifactPayload payload;
  payload.index.job_id=42; payload.index.printer_name="打印机甲"; payload.index.capture_id="unique";
  payload.index.job_print_ticket="job.unique.pt.xml"; payload.job_print_ticket_xml="<ticket/>";
  payload.index.fixed_page="page.unique.fpage"; payload.fixed_page_xml="<FixedPage/>"; payload.index.fixed_page_state="reported";
  EXPECT_TRUE(writer.enqueue(payload)); writer.stop(); EXPECT_TRUE(writer.drained());
  EXPECT_EQ(read(root/"pages"/payload.index.job_print_ticket),"<ticket/>");
  EXPECT_EQ(read(root/"pages"/payload.index.fixed_page),"<FixedPage/>");
  EXPECT_TRUE(fs::exists(storage::page_artifact_archive_path({.output_directory=root},payload.index)));
  EXPECT_NE(read(root/"page-artifacts.json").find("page.unique.fpage"),std::string::npos);
}
TEST_F(CaptureReliability, PageWriterRejectsFakeReferencesAndUnsafeNames)
{
  storage::AsyncPageArtifactWriter writer({.output_directory=root}); writer.start();
  storage::PageArtifactPayload payload; payload.index.fixed_page="missing.fpage";
  EXPECT_FALSE(writer.enqueue(payload)); payload.fixed_page_xml="data"; payload.index.fixed_page="../escape.fpage";
  EXPECT_FALSE(writer.enqueue(payload)); writer.stop(); EXPECT_FALSE(fs::exists(root/"page-artifacts.json"));
}
TEST_F(CaptureReliability, BusyInboxRootDoesNotStarveOtherDirectories)
{
  const auto other = root / "other";
  fs::create_directories(other);
  auto pending = message(); pending.message_id = "pending";
  storage::write_text_file_atomic(bridge::serialize_pt_message(pending), root / "first.pt-message.json");
  auto ready = message(); ready.message_id = "ready";
  storage::write_text_file_atomic(bridge::serialize_pt_message(ready), other / "second.pt-message.json");
  const auto result = bridge::consume_pt_inbox({root, other}, [](const auto& item) {
    return item.message_id == "ready";
  }, 1);
  EXPECT_EQ(result.pending, 1u);
  EXPECT_EQ(result.delivered, 1u);
  EXPECT_TRUE(fs::exists(root / "first.pt-message.json.claim"));
  EXPECT_FALSE(fs::exists(other / "second.pt-message.json.claim"));
}
TEST_F(CaptureReliability, PageIndexWaitsForPayloadRecovery)
{
  std::atomic_bool fail{true};
  storage::AsyncPageArtifactWriter writer({.output_directory=root,.worker_hook=[&](auto stage,const auto& path) {
    if(fail && path.filename()=="page.fpage" && stage==storage::AtomicWriteStage::Replace) throw std::runtime_error("locked page");
  }});
  storage::PageArtifactPayload payload; payload.index.job_id=42; payload.index.printer_name="Printer";
  payload.index.fixed_page="page.fpage"; payload.fixed_page_xml="<FixedPage/>";
  writer.start(); ASSERT_TRUE(writer.enqueue(payload));
  EXPECT_TRUE(wait_for([&] {return !writer.last_error().empty();}));
  EXPECT_FALSE(fs::exists(root/"page-artifacts.json")); EXPECT_EQ(writer.queue_depth(),1u);
  fail=false; EXPECT_TRUE(wait_for([&] {return writer.drained();})); writer.stop();
  EXPECT_EQ(read(root/"pages/page.fpage"),"<FixedPage/>"); EXPECT_TRUE(fs::exists(root/"page-artifacts.json"));
}
TEST_F(CaptureReliability, SnapshotWriterKeepsFailedItemAndLegacyCaptureUntouched)
{
  const auto legacy=root/"jobs/HP_A/job-42.json"; storage::write_text_file_atomic("legacy bytes",legacy);
  core::JobSnapshot job; job.job_id=42; job.printer_name="HP A";
  core::MetadataSnapshot metadata; metadata.job_id=42; metadata.printer_name=job.printer_name;
  {
    storage::AsyncJobSnapshotWriter writer({.output_directory=root,.worker_hook=[](auto,const auto&) {throw std::runtime_error("disk offline");}});
    writer.start(); ASSERT_TRUE(writer.enqueue(job,metadata)); writer.stop(); EXPECT_FALSE(writer.drained());
  }
  storage::AsyncJobSnapshotWriter recovered({.output_directory=root}); recovered.start(); recovered.stop();
  EXPECT_TRUE(recovered.drained()); EXPECT_TRUE(fs::exists(storage::job_snapshot_archive_path({.output_directory=root},job)));
  EXPECT_EQ(read(legacy),"legacy bytes");
}
TEST_F(CaptureReliability, PhysicalIdentityUsesTheSameUtcPrecisionAsWatcher)
{
  JOB_INFO_2W info{}; wchar_t printer[]=L"Printer", user[]=L"alice", document[]=L"file.pdf";
  info.pPrinterName=printer; info.pUserName=user; info.pDocument=document; info.JobId=42;
  info.Submitted={2026,10,0,24,1,2,3,999};
  const auto snapshot=filter::physical_snapshot(info);
  EXPECT_EQ(snapshot.submit_time_utc,"2026-10-24T01:02:03");
  EXPECT_EQ(common::physical_job_identity(snapshot),"Printer\x1f" "alice\x1f" "file.pdf\x1f" "2026-10-24T01:02:03");
}
}
