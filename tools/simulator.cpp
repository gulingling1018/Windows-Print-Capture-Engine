/// WPCE Virtual Print Simulator
///
/// Creates real Win32 spooler print jobs against a target printer and
/// manipulates their lifecycle to exercise the watcher under varied
/// scenarios: normal flow, fast-disappear, concurrency, error, cancel,
/// stall, parameter variety.
///
/// Usage:
///   wpce_sim.exe --target-printer <name> --scenario <name> [--job-count N]
///
/// Scenarios:
///   normal           — jobs complete normally via NUL port
///   fast-disappear   — jobs cancelled immediately after submission
///   high-concurrency — many jobs submitted rapidly
///   cancel-mid       — jobs paused briefly then cancelled
///   stall            — jobs paused indefinitely (watcher detects stall)
///   param-variety    — jobs with varied DEVMODE: copies, duplex, color, paper
///   printer-error    — jobs to fault printer (non-routable IP, triggers ERROR)
///   all              — run all scenarios in sequence

#ifdef _WIN32
#include <windows.h>
#include <winspool.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

// ── RAII printer handle ───────────────────────────────────────────────

class PrinterHandle
{
 public:
  explicit PrinterHandle(const std::wstring& name)
  {
    if (!OpenPrinterW(const_cast<LPWSTR>(name.c_str()), &handle_, nullptr))
    {
      throw std::runtime_error("OpenPrinterW failed: " + std::to_string(GetLastError()));
    }
  }

  ~PrinterHandle()
  {
    if (handle_ != nullptr) ClosePrinter(handle_);
  }

  PrinterHandle(const PrinterHandle&) = delete;
  PrinterHandle& operator=(const PrinterHandle&) = delete;

  HANDLE get() const noexcept { return handle_; }

 private:
  HANDLE handle_{nullptr};
};

// ── Win32 error formatting ────────────────────────────────────────────

std::string format_win32_error(DWORD error, std::string_view operation)
{
  LPSTR message = nullptr;
  DWORD size = FormatMessageA(
    FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
    nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
    reinterpret_cast<LPSTR>(&message), 0, nullptr);

  std::ostringstream out;
  out << operation << " failed: Win32 error " << error;
  if (size > 0 && message != nullptr)
  {
    std::string text(message, size);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == '.'))
      text.pop_back();
    out << " — " << text;
  }
  if (message != nullptr) LocalFree(message);
  return out.str();
}

// ── Conversion helpers ────────────────────────────────────────────────

std::wstring widen(std::string_view s)
{
  if (s.empty()) return {};
  int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (len <= 0) return {};
  std::wstring result(static_cast<std::size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), result.data(), len);
  return result;
}

std::string narrow(const wchar_t* ws)
{
  if (!ws || !ws[0]) return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 1) return {};
  std::string result(static_cast<std::size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, ws, -1, result.data(), len, nullptr, nullptr);
  if (!result.empty() && result.back() == '\0') result.pop_back();
  return result;
}

// ── Job submission ────────────────────────────────────────────────────

/// Submit a synthetic print job and return its JobId.
/// Returns 0 on failure.
DWORD submit_print_job(HANDLE printer, const std::string& document_name, const std::string& data)
{
  DOC_INFO_1W docInfo{};
  std::wstring wDocName = widen(document_name);
  docInfo.pDocName = const_cast<LPWSTR>(wDocName.c_str());
  docInfo.pOutputFile = nullptr;  // use printer's port
  std::wstring wDataType = L"RAW";
  docInfo.pDatatype = const_cast<LPWSTR>(wDataType.c_str());

  DWORD jobId = StartDocPrinterW(printer, 1, reinterpret_cast<LPBYTE>(&docInfo));
  if (jobId == 0)
  {
    std::cerr << "  ! StartDocPrinterW error: " << GetLastError() << '\n';
    return 0;
  }

  if (!StartPagePrinter(printer))
  {
    std::cerr << "  ! StartPagePrinter error: " << GetLastError() << '\n';
    EndDocPrinter(printer);
    return jobId;  // job exists but has no pages
  }

  DWORD written = 0;
  WritePrinter(printer, const_cast<char*>(data.data()), static_cast<DWORD>(data.size()), &written);

  EndPagePrinter(printer);
  EndDocPrinter(printer);

  return jobId;
}

/// Create a job that hangs in spooling (no EndDocPrinter).
/// Used for stall-detection testing — the job stays in spooling/printing
/// until explicitly cancelled or unpaused.
DWORD submit_hanging_job(HANDLE printer, const std::string& document_name, const std::string& data)
{
  DOC_INFO_1W docInfo{};
  std::wstring wDocName = widen(document_name);
  docInfo.pDocName = const_cast<LPWSTR>(wDocName.c_str());
  docInfo.pOutputFile = nullptr;
  std::wstring wDataType = L"RAW";
  docInfo.pDatatype = const_cast<LPWSTR>(wDataType.c_str());

  DWORD jobId = StartDocPrinterW(printer, 1, reinterpret_cast<LPBYTE>(&docInfo));
  if (jobId == 0)
  {
    std::cerr << "  ! StartDocPrinterW (hanging) error: " << GetLastError() << '\n';
    return 0;
  }

  if (!StartPagePrinter(printer))
  {
    std::cerr << "  ! StartPagePrinter (hanging) error: " << GetLastError() << '\n';
    EndDocPrinter(printer);
    return jobId;
  }

  DWORD written = 0;
  WritePrinter(printer, const_cast<char*>(data.data()), static_cast<DWORD>(data.size()), &written);
  // Intentionally NOT calling EndPagePrinter/EndDocPrinter —
  // the job stays in spooling until paused/cancelled/timed out.

  return jobId;
}

/// Submit a print job with a specific DEVMODE configuration.
DWORD submit_print_job_with_devmode(HANDLE printer,
                                     const std::string& document_name,
                                     const std::string& data,
                                     std::int16_t copies,
                                     std::int16_t orientation,
                                     const std::string& paper_size_name)
{
  // Get driver-supported paper sizes.
  DWORD needed = DocumentPropertiesW(nullptr, printer, const_cast<LPWSTR>(widen("WPCE Sim Printer").c_str()),
                                      nullptr, nullptr, 0);
  if (needed == 0)
  {
    // Fallback: submit without custom DEVMODE.
    return submit_print_job(printer, document_name, data);
  }

  std::vector<BYTE> devmode_buffer(needed);
  DEVMODEW* dm = reinterpret_cast<DEVMODEW*>(devmode_buffer.data());
  dm->dmSize = sizeof(DEVMODEW);
  dm->dmDriverExtra = 0;

  // Get default DEVMODE from driver.
  if (DocumentPropertiesW(nullptr, printer, const_cast<LPWSTR>(widen("WPCE Sim Printer").c_str()),
                           dm, dm, DM_OUT_BUFFER) != IDOK)
  {
    return submit_print_job(printer, document_name, data);
  }

  // Apply custom fields.
  dm->dmFields = DM_COPIES | DM_ORIENTATION | DM_PAPERSIZE | DM_DUPLEX | DM_COLOR;
  dm->dmCopies = copies;
  dm->dmOrientation = orientation;
  dm->dmDuplex = DMDUP_SIMPLEX;
  dm->dmColor = DMCOLOR_COLOR;

  // Paper size lookup.
  struct PaperEntry { const char* name; short id; };
  static const PaperEntry papers[] = {
    {"A4", DMPAPER_A4}, {"A3", DMPAPER_A3}, {"Letter", DMPAPER_LETTER},
    {"Legal", DMPAPER_LEGAL}, {"A5", DMPAPER_A5}, {"B5", DMPAPER_B5},
  };
  dm->dmPaperSize = DMPAPER_A4;  // default
  for (const auto& p : papers)
  {
    if (paper_size_name == p.name) { dm->dmPaperSize = p.id; break; }
  }

  // Merge into driver's DEVMODE.
  if (DocumentPropertiesW(nullptr, printer, const_cast<LPWSTR>(widen("WPCE Sim Printer").c_str()),
                           dm, dm, DM_IN_BUFFER | DM_OUT_BUFFER) != IDOK)
  {
    return submit_print_job(printer, document_name, data);
  }

  DOC_INFO_1W docInfo{};
  std::wstring wDocName = widen(document_name);
  docInfo.pDocName = const_cast<LPWSTR>(wDocName.c_str());
  docInfo.pOutputFile = nullptr;
  std::wstring wDataType = L"RAW";
  docInfo.pDatatype = const_cast<LPWSTR>(wDataType.c_str());

  DWORD jobId = StartDocPrinterW(printer, 1, reinterpret_cast<LPBYTE>(&docInfo));
  if (jobId == 0) return 0;

  StartPagePrinter(printer);
  DWORD written = 0;
  WritePrinter(printer, const_cast<char*>(data.data()), static_cast<DWORD>(data.size()), &written);
  EndPagePrinter(printer);
  EndDocPrinter(printer);

  return jobId;
}

// ── Job manipulation ──────────────────────────────────────────────────

void cancel_job(HANDLE printer, DWORD jobId)
{
  if (!SetJobW(printer, jobId, 0, nullptr, JOB_CONTROL_CANCEL))
  {
    DWORD err = GetLastError();
    // ERROR_INVALID_PARAMETER (87) = job already gone/terminal — fine.
    if (err != ERROR_INVALID_PARAMETER)
    {
      std::cerr << "  ! SetJobW(CANCEL) job " << jobId << ": " << GetLastError() << '\n';
    }
  }
}

void pause_job(HANDLE printer, DWORD jobId)
{
  if (!SetJobW(printer, jobId, 0, nullptr, JOB_CONTROL_PAUSE))
  {
    std::cerr << "  ! SetJobW(PAUSE) job " << jobId << ": " << GetLastError() << '\n';
  }
}

// ── Scenario runners ──────────────────────────────────────────────────

struct ScenarioConfig
{
  std::wstring target_printer;
  std::uint32_t job_count{5};
};

int run_normal(const ScenarioConfig& config)
{
  std::cout << "\n=== Scenario: Normal Flow ===\n";
  std::cout << "Submitting " << config.job_count << " jobs to \""
            << narrow(config.target_printer.c_str()) << "\"...\n";

  PrinterHandle printer(config.target_printer);

  std::vector<DWORD> jobIds;
  for (std::uint32_t i = 0; i < config.job_count; ++i)
  {
    std::ostringstream docName;
    docName << "normal-test-" << (i + 1) << ".txt";
    std::ostringstream data;
    data << "WPCE Normal Test Job " << (i + 1) << "\n"
         << "Pages: " << ((i % 3) + 1) << "\n"
         << "Timestamp: " << std::chrono::system_clock::now().time_since_epoch().count() << "\n";

    DWORD id = submit_print_job(printer.get(), docName.str(), data.str());
    if (id != 0)
    {
      jobIds.push_back(id);
      std::cout << "  [" << id << "] submitted: " << docName.str() << '\n';
    }
  }

  std::cout << "Submitted " << jobIds.size() << " jobs. They should complete via NUL port.\n";
  return 0;
}

int run_fast_disappear(const ScenarioConfig& config)
{
  std::cout << "\n=== Scenario: Fast-Disappearing Jobs ===\n";
  std::cout << "Jobs will be cancelled immediately after submission.\n";

  PrinterHandle printer(config.target_printer);
  std::vector<DWORD> jobIds;

  for (std::uint32_t i = 0; i < config.job_count; ++i)
  {
    std::ostringstream docName;
    docName << "fast-disappear-" << (i + 1) << ".txt";
    std::string data = "WPCE Fast Disappear Test " + std::to_string(i + 1) + "\n";

    DWORD id = submit_print_job(printer.get(), docName.str(), data);
    if (id != 0)
    {
      jobIds.push_back(id);
      std::cout << "  [" << id << "] created: " << docName.str() << '\n';
    }
  }

  // Cancel immediately — before the watcher's 250ms poll cycle.
  std::cout << "Cancelling all " << jobIds.size() << " jobs immediately...\n";
  for (DWORD id : jobIds)
  {
    cancel_job(printer.get(), id);
  }

  return 0;
}

int run_high_concurrency(const ScenarioConfig& config)
{
  std::cout << "\n=== Scenario: High Concurrency ===\n";
  std::cout << "Submitting " << config.job_count << " jobs in rapid succession.\n";

  PrinterHandle printer(config.target_printer);
  const auto start = std::chrono::steady_clock::now();
  std::uint32_t submitted = 0;

  for (std::uint32_t i = 0; i < config.job_count; ++i)
  {
    std::ostringstream docName;
    docName << "concurrent-" << (i + 1) << ".txt";
    std::string data = "WPCE Concurrency Test " + std::to_string(i + 1) + "\n";

    DWORD id = submit_print_job(printer.get(), docName.str(), data);
    if (id != 0) ++submitted;
  }

  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - start).count();
  std::cout << "Submitted " << submitted << '/' << config.job_count
            << " jobs in " << elapsed << " ms\n";
  return 0;
}

int run_printer_error(const ScenarioConfig& config)
{
  std::cout << "\n=== Scenario: Printer Error ===\n";
  std::cout << "Submitting to fault printer (non-routable IP)...\n";

  PrinterHandle printer(config.target_printer);
  std::vector<DWORD> jobIds;

  for (std::uint32_t i = 0; i < config.job_count; ++i)
  {
    std::ostringstream docName;
    docName << "error-test-" << (i + 1) << ".txt";
    std::string data = "WPCE Error Test " + std::to_string(i + 1) + "\n";

    DWORD id = submit_print_job(printer.get(), docName.str(), data);
    if (id != 0)
    {
      jobIds.push_back(id);
      std::cout << "  [" << id << "] sent to fault printer: " << docName.str() << '\n';
    }
  }

  std::cout << "Submitted " << jobIds.size() << " jobs to fault printer.\n";
  std::cout << "Windows will eventually set JOB_STATUS_ERROR on these.\n";
  return 0;
}

int run_cancel_mid(const ScenarioConfig& config)
{
  std::cout << "\n=== Scenario: Mid-Flow Cancellation ===\n";

  PrinterHandle printer(config.target_printer);
  std::vector<DWORD> jobIds;

  for (std::uint32_t i = 0; i < config.job_count; ++i)
  {
    std::ostringstream docName;
    docName << "cancel-mid-" << (i + 1) << ".txt";
    std::string data = "WPCE Mid-Cancel Test " + std::to_string(i + 1) + "\n";

    DWORD id = submit_print_job(printer.get(), docName.str(), data);
    if (id != 0)
    {
      jobIds.push_back(id);
      std::cout << "  [" << id << "] created: " << docName.str() << '\n';
    }
  }

  // Wait briefly so the spooler has time to process them, then cancel.
  std::cout << "Waiting 500ms then cancelling...\n";
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  for (DWORD id : jobIds)
  {
    cancel_job(printer.get(), id);
  }

  return 0;
}

int run_stall(const ScenarioConfig& config)
{
  std::cout << "\n=== Scenario: Stall Detection ===\n";
  std::cout << "Creating hanging jobs (no EndDocPrinter) — watcher should detect stall.\n";

  PrinterHandle printer(config.target_printer);
  std::vector<DWORD> jobIds;

  for (std::uint32_t i = 0; i < config.job_count; ++i)
  {
    std::ostringstream docName;
    docName << "stall-test-" << (i + 1) << ".txt";
    std::string data = "WPCE Stall Test " + std::to_string(i + 1) + "\n";

    DWORD id = submit_hanging_job(printer.get(), docName.str(), data);
    if (id != 0)
    {
      jobIds.push_back(id);
      std::cout << "  [" << id << "] hanging in spooling: " << docName.str() << '\n';
    }
  }

  // Pause to prevent the spooler from completing them.
  for (DWORD id : jobIds)
  {
    pause_job(printer.get(), id);
  }

  std::cout << "Jobs paused. Watcher with --stall-timeout should detect stalled.\n";
  return 0;
}

int run_param_variety(const ScenarioConfig& config)
{
  std::cout << "\n=== Scenario: Parameter Variety ===\n";

  PrinterHandle printer(config.target_printer);

  struct ParamJob
  {
    std::string doc_name;
    std::int16_t copies;
    std::int16_t orientation;  // 1=portrait, 2=landscape
    std::string paper;
  };

  const ParamJob jobs[] = {
    {"param-A4-1copy-portrait.txt",     1, 1, "A4"},
    {"param-Letter-3copy-landscape.txt",3, 2, "Letter"},
    {"param-Legal-2copy-portrait.txt",  2, 1, "Legal"},
    {"param-A3-1copy-landscape.txt",    1, 2, "A3"},
    {"param-A5-5copy-portrait.txt",     5, 1, "A5"},
  };

  for (const auto& pj : jobs)
  {
    std::string data = "WPCE Param Test — copies=" + std::to_string(pj.copies) +
                       " orientation=" + (pj.orientation == 1 ? "portrait" : "landscape") +
                       " paper=" + pj.paper + "\n";

    DWORD id = submit_print_job_with_devmode(printer.get(), pj.doc_name, data,
                                              pj.copies, pj.orientation, pj.paper);
    if (id != 0)
    {
      std::cout << "  [" << id << "] " << pj.doc_name
                << " copies=" << pj.copies
                << " orient=" << (pj.orientation == 1 ? "portrait" : "landscape")
                << " paper=" << pj.paper << '\n';
    }
  }

  return 0;
}

using ScenarioFn = int(*)(const ScenarioConfig&);

struct ScenarioEntry
{
  std::string_view name;
  ScenarioFn runner;
  std::string_view description;
  std::string_view target_printer;  // "sim" or "fault" — which printer to use
};

}  // namespace

// ── Main ──────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
#ifdef _WIN32
  SetConsoleCP(CP_UTF8);
  SetConsoleOutputCP(CP_UTF8);
#endif

  std::string scenario_name = "normal";
  std::string target_printer_arg;
  std::uint32_t job_count = 0;  // 0 = use default

  for (int i = 1; i < argc; ++i)
  {
    std::string_view arg = argv[i];
    if (arg == "--scenario" && i + 1 < argc)
      scenario_name = argv[++i];
    else if (arg == "--target-printer" && i + 1 < argc)
      target_printer_arg = argv[++i];
    else if (arg == "--job-count" && i + 1 < argc)
    {
      try { job_count = static_cast<std::uint32_t>(std::stoul(argv[++i])); } catch (...) {}
    }
    else if (arg == "--help" || arg == "-h")
    {
      std::cout << "WPCE Virtual Print Simulator\n\n"
                << "Usage: wpce_sim.exe [options]\n\n"
                << "Options:\n"
                << "  --scenario <name>     normal | fast-disappear | high-concurrency |\n"
                << "                        printer-error | cancel-mid | stall |\n"
                << "                        param-variety | all\n"
                << "  --target-printer <n>  Printer name (default: WPCE Sim Printer)\n"
                << "  --job-count <N>       Override default job count\n\n"
                << "Test printers available:\n"
                << "  WPCE Sim Printer     — nul: port, silent completion\n"
                << "  WPCE Fault Printer   — non-routable IP, triggers error\n";
      return 0;
    }
  }

  // Default printers.
  const std::string kSimPrinter = target_printer_arg.empty() ? "WPCE Sim Printer" : target_printer_arg;
  const std::string kFaultPrinter = "WPCE Fault Printer";

  const ScenarioEntry all_scenarios[] = {
    {"normal",           run_normal,           "Jobs complete normally via NUL",              kSimPrinter},
    {"fast-disappear",   run_fast_disappear,   "Jobs cancelled before watcher can enumerate", kSimPrinter},
    {"high-concurrency", run_high_concurrency, "50+ jobs in rapid succession",                kSimPrinter},
    {"printer-error",    run_printer_error,    "Jobs to fault printer trigger ERROR phase",   kFaultPrinter},
    {"cancel-mid",       run_cancel_mid,       "Jobs cancelled mid-spooling",                 kSimPrinter},
    {"stall",            run_stall,            "Jobs paused — watcher detects stall",          kSimPrinter},
    {"param-variety",    run_param_variety,    "Varied copies/duplex/color/paper",            kSimPrinter},
  };

  std::cout << "╔═══════════════════════════════════════╗\n"
            << "║  WPCE Virtual Print Simulator         ║\n"
            << "╚═══════════════════════════════════════╝\n\n";

  int total_passed = 0;
  int total_ran = 0;

  if (scenario_name == "all")
  {
    for (const auto& scen : all_scenarios)
    {
      ScenarioConfig scfg;
      scfg.target_printer = widen(scen.target_printer);
      scfg.job_count = job_count > 0 ? job_count
                     : (scen.name == "high-concurrency" ? 50u : 5u);
      try
      {
        int rc = scen.runner(scfg);
        if (rc == 0) ++total_passed;
        ++total_ran;
      }
      catch (const std::exception& e)
      {
        std::cerr << "  ! Scenario \"" << scen.name << "\" threw: " << e.what() << '\n';
        ++total_ran;
      }
      // Brief pause between scenarios so the watcher can catch up.
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
  else
  {
    // Find the requested scenario.
    bool found = false;
    for (const auto& scen : all_scenarios)
    {
      if (scen.name == scenario_name)
      {
        ScenarioConfig scfg;
        scfg.target_printer = widen(scen.target_printer);
        scfg.job_count = job_count > 0 ? job_count
                       : (scen.name == "high-concurrency" ? 50u : 5u);
        try
        {
          int rc = scen.runner(scfg);
          if (rc == 0) ++total_passed;
          ++total_ran;
        }
        catch (const std::exception& e)
        {
          std::cerr << "  ! Scenario \"" << scen.name << "\" threw: " << e.what() << '\n';
          ++total_ran;
        }
        found = true;
        break;
      }
    }
    if (!found)
    {
      std::cerr << "Unknown scenario: " << scenario_name << '\n';
      std::cerr << "Valid: normal, fast-disappear, high-concurrency, printer-error, "
                   "cancel-mid, stall, param-variety, all\n";
      return 2;
    }
  }

  std::cout << "\n═══ Simulator Summary ═══\n"
            << "Scenarios ran:    " << total_ran << '\n'
            << "Scenarios passed: " << total_passed << '\n'
            << "\nWatcher should have captured jobs from these scenarios.\n"
            << "Check lifecycle-summary.json for actual results.\n";

  return (total_passed == total_ran) ? 0 : 1;
}
