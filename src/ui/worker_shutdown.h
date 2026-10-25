#pragma once
#include <QDeadlineTimer>
#include <QThread>
#include <vector>

namespace wpce::ui
{
// A timeout retains the worker so that its owner can retry without killing it.
inline bool join_worker_threads(std::vector<QThread*>& workers, unsigned long timeoutMs)
{
  const QDeadlineTimer deadline(static_cast<qint64>(timeoutMs));
  std::vector<QThread*> pending;
  for (auto* worker : workers)
  {
    if (!worker) continue;
    worker->quit();
    if (!worker->wait(deadline)) pending.push_back(worker);
    else delete worker;
  }
  workers.swap(pending);
  return workers.empty();
}
}
