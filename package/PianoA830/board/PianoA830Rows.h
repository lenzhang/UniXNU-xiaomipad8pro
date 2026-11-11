// SPDX-License-Identifier: BSD-2-Clause-Patent
// The status rows' console rule (board/PianoA830.cpp boot_progress_gpu_rows), host-testable (tooling/k27/run-a830-host-tests.py): once
// the GPU runs, a row is printed when it changed and 10 s have passed, or at once when row 0's state changed. Row 0's state is its
// second word (RUN, LOST, S<n>.<step> during the bring-up) and its " f<n>" (faults) and " h<n>" (hangs) fields; the clock, the busy
// share, the levels, the submission count and the temperature are not.
#pragma once
#include <stddef.h>

enum : size_t { PianoA830RowKeyBytes = 40 };

// key: NUL-terminated, at most PianoA830RowKeyBytes - 2 characters. row: at most bytes characters, NUL-terminated or not.
static inline void PianoA830RowZeroKey(const char *row, size_t bytes, char key[PianoA830RowKeyBytes])
{
  size_t n = 0, i = 0;
  const size_t room = PianoA830RowKeyBytes - 2;
  while (i < bytes && row[i] && row[i] != ' ') ++i;
  while (i < bytes && row[i] == ' ') ++i;
  while (i < bytes && row[i] && row[i] != ' ' && n < 12) key[n++] = row[i++];
  for (; i + 2 < bytes && row[i]; ++i) {
    if (row[i] != ' ' || (row[i + 1] != 'f' && row[i + 1] != 'h') || row[i + 2] < '0' || row[i + 2] > '9') continue;
    if (n < room) key[n++] = row[i + 1];
    for (size_t j = i + 2; j < bytes && row[j] >= '0' && row[j] <= '9' && n < room; ++j) key[n++] = row[j];
  }
  key[n] = 0;
}

// Whether row 0 went from was to is in a way the console must show at once.
static inline bool PianoA830RowZeroStateChanged(const char *was, const char *is, size_t bytes)
{
  char a[PianoA830RowKeyBytes], b[PianoA830RowKeyBytes];
  PianoA830RowZeroKey(was, bytes, a);
  PianoA830RowZeroKey(is, bytes, b);
  for (size_t i = 0; i < PianoA830RowKeyBytes; ++i) {
    if (a[i] != b[i]) return true;
    if (!a[i]) return false;
  }
  return false;
}
