"""Local estimate anchored to verified account readings. Stores counts only."""

import contextlib
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import re
import time

try:
    import fcntl
except ImportError:
    fcntl = None


DAY = 86400
HISTORY_LIMIT = 28
ACCURACY_LIMIT = 120
ACCURACY_WINDOW = 5
TURN_WEIGHT = 256  # Heuristic for context and work absent from the transcript.
MAX_LOCAL_RATE = 3.0  # Percentage points per 1,000 character equivalents.
MAX_SHARED_RATE = 10.0  # Percentage points per day.
COUNTERS = ('sent', 'reply', 'turns')


def workload(counts):
    return (counts['sent'] + TURN_WEIGHT * counts['turns']) / 1000, counts['reply'] / 1000


def learn(history, legacy_rate):
    """Fit a small regularized model. Older windows retain progressively less weight."""
    idle = [(row, 0.88 ** (len(history) - i - 1)) for i, row in enumerate(history)
            if not any(row[k] for k in COUNTERS) and row['seconds'] >= 2 * 3600]
    shared = 0.0
    if sum(row['points'] > 0 for row, _ in idle) >= 2:
        # Subtract one point of rounding/delay slack across the independent intervals.
        exposure = sum(row['seconds'] / DAY * weight for row, weight in idle)
        gain = sum(min(row['points'], MAX_SHARED_RATE * row['seconds'] / DAY) * weight for row, weight in idle)
        shared = max(0.0, (gain - 1) / exposure)

    rows = []
    for i, row in enumerate(history):
        sent, reply = workload(row)
        size = sent + reply
        if size >= 1:
            days = row['seconds'] / DAY
            points = max(0, row['points'] - shared * days)
            weight = 0.88 ** (len(history) - i - 1) * size / (size + 3)
            rows.append((sent / size, reply / size, points / size, weight, size))

    positive = sorted(rate for _, _, rate, _, _ in rows if rate > 0)
    if not positive and not legacy_rate:
        return None, shared
    if not rows:
        return None, shared  # Keep the legacy calculation until there is directional evidence.
    prior = min(MAX_LOCAL_RATE, legacy_rate * 1000 if legacy_rate else positive[(len(positive) - 1) // 2])
    center = max(prior, min(MAX_LOCAL_RATE, positive[(len(positive) - 1) // 2])) if positive else prior
    # Shrink both directions toward the same prior until there is enough varied evidence.
    aa = bb = 0.35
    ab = 0.0
    ay = by = 0.35 * prior
    for sent, reply, rate, weight, size in rows:
        rate = min(rate, MAX_LOCAL_RATE, max(2.5 * center, center + 2 / size))
        aa += weight * sent * sent
        bb += weight * reply * reply
        ab += weight * sent * reply
        ay += weight * sent * rate
        by += weight * reply * rate
    # Solve the two-variable fit, including nonnegative boundary solutions.
    det = aa * bb - ab * ab
    sent = (bb * ay - ab * by) / det
    reply = (aa * by - ab * ay) / det
    if sent < 0:
        sent, reply = 0, by / bb
    elif reply < 0:
        sent, reply = ay / aa, 0
    return [min(MAX_LOCAL_RATE, sent), min(MAX_LOCAL_RATE, reply)], shared


class Usage:
    def __init__(self, directory='/var/lib/muse-core2-bridge'):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.path = self.directory / 'usage-estimate.json'

    def boot(self):
        try:
            return Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        except OSError:
            return None

    @contextlib.contextmanager
    def state(self, write=True):
        with (self.directory / 'usage-estimate.lock').open('a+') as lock:
            if fcntl:
                fcntl.flock(lock, fcntl.LOCK_EX)
            try:
                if self.path.exists():
                    state = json.loads(self.path.read_text(encoding='utf8'))
                else:
                    state = dict(v=2, chars=0, last_seq=None, anchor=None, rate=None)
                if state['v'] == 1:
                    state['v'] = 2
                    # The old aggregate and calibration remain usable, but cannot be split by role.
                    state.pop('sample', None)
                    anchor = state['anchor']
                    if anchor and anchor['chars'] == state['chars']:
                        anchor['counts'] = dict.fromkeys(COUNTERS, 0)
                state.setdefault('counts', dict.fromkeys(COUNTERS, 0))
                state.setdefault('history', [])
                state.setdefault('local', None)
                state.setdefault('shared', 0.0)
                yield state
                if write:
                    tmp = self.path.with_suffix('.tmp')
                    with tmp.open('w', encoding='utf8') as file:
                        if os.name == 'posix':
                            os.fchmod(file.fileno(), 0o600)
                        json.dump(state, file)
                        file.flush()
                        os.fsync(file.fileno())
                    os.replace(tmp, self.path)
            finally:
                if fcntl:
                    fcntl.flock(lock, fcntl.LOCK_UN)

    def sync(self, db):
        with self.state() as state:
            last = db.execute('SELECT coalesce(max(seq),0) FROM messages').fetchone()[0]
            cursor = state['last_seq']
            if cursor is not None and last > cursor:
                rows = db.execute(
                    "SELECT m.role,count(*),coalesce(sum(length(m.text)),0) FROM messages m "
                    "WHERE m.seq>? AND m.seq<=? AND m.role IN ('user','assistant') AND NOT EXISTS("
                    "SELECT 1 FROM settings x WHERE x.key='v12_fixture:'||m.chat AND x.value='owned') GROUP BY m.role",
                    (cursor, last),
                )
                for role, messages, chars in rows:
                    state['chars'] += chars
                    state['counts']['sent' if role == 'user' else 'reply'] += chars
                    if role == 'user':
                        state['counts']['turns'] += messages
            state['last_seq'] = max(last, cursor or 0)

    def calibrate(self, observation):
        used, reset, checked = (observation.get(k) for k in ('used', 'reset', 'checked'))
        if type(used) is not int or not 0 <= used <= 100:
            raise ValueError('Usage must be a whole percent from the visible website')
        if not isinstance(reset, str) or not re.fullmatch(r'[A-Za-z]{3,9} [0-9]{1,2}', reset):
            raise ValueError('Invalid visible reset date')
        if not isinstance(checked, str) or not re.fullmatch(r'[A-Za-z]{3} [0-9]{1,2} [0-9]{2}:[0-9]{2}', checked):
            raise ValueError('Invalid check label')
        observed_at = observation.get('observed_at', time.time())
        if type(observed_at) not in (int, float) or not 0 < observed_at < 4102444800:
            raise ValueError('Invalid observation time')
        with self.state() as state:
            anchor = state['anchor']
            if anchor and observed_at <= anchor.get('observed_at', 0):
                if observed_at < anchor['observed_at'] or used != anchor['used'] or reset != anchor['reset']:
                    raise ValueError('Older or conflicting observation rejected')
            else:
                self.score(state, observation, used, reset, observed_at)
                self.observe(state, anchor, used, reset, observed_at)
                state['anchor'] = dict(used=used, reset=reset, checked=checked, chars=state['chars'],
                                       counts=state['counts'].copy(), received=time.time(), observed_at=observed_at,
                                       boot=self.boot(), tick=time.monotonic())
        return self.snapshot()

    def score(self, state, observation, used, reset, observed_at):
        if 'prediction' in observation:
            prediction = observation['prediction']
            timing = 'check_start'
        else:
            prior = self.estimate(state)
            prediction = dict(used=prior['used'], reset=prior['reset']) if prior else None
            timing = 'before_apply'
        if (not isinstance(prediction, dict) or type(prediction.get('used')) is not int
                or not 0 <= prediction['used'] <= 100 or not isinstance(prediction.get('reset'), str)
                or not re.fullmatch(r'[A-Za-z]{3,9} [0-9]{1,2}', prediction['reset'])):
            prediction = None
        estimate = prediction['used'] if prediction else None
        window = prediction['reset'] if prediction else None
        error = estimate - used if prediction and window == reset else None
        entry = dict(observed_at=observed_at, reset=reset, verified=used, estimate=estimate,
                     estimate_reset=window, timing=timing, error_pp=error)
        state['accuracy'] = (state.get('accuracy', []) + [entry])[-ACCURACY_LIMIT:]

    @staticmethod
    def accuracy_summary(history):
        errors = [row['error_pp'] for row in history if row['timing'] == 'check_start' and row['error_pp'] is not None]
        latest = history[-1] if history else None
        last_error = latest['error_pp'] if latest and latest['timing'] == 'check_start' else None
        size = ACCURACY_WINDOW

        def measure(values):
            return dict(count=len(values), mean_absolute_pp=round(sum(map(abs, values)) / len(values), 1),
                        mean_signed_pp=round(sum(values) / len(values), 1)) if values else None

        recent = measure(errors[-size:])
        previous = measure(errors[-2*size:-size]) if len(errors) >= 2*size else None
        return dict(scored=len(errors), latest_error_pp=last_error, recent=recent, previous=previous)

    def accuracy_report(self):
        with self.state(write=False) as state:
            history = state.get('accuracy', [])
            return dict(summary=self.accuracy_summary(history), checks=history)

    @staticmethod
    def observe(state, anchor, used, reset, observed_at):
        mark = state.get('mark', anchor)
        if (not anchor or not mark or 'counts' not in mark or reset != anchor['reset'] or used < anchor['used']
                or observed_at - mark['observed_at'] > 7 * DAY):
            # Re-anchor when the window changes or Muse corrects its reading. Keep fitted rates.
            state['mark'] = dict(used=used, observed_at=observed_at, counts=state['counts'].copy())
            return
        counts = {key:state['counts'][key] - mark['counts'][key] for key in COUNTERS}
        seconds = observed_at - mark['observed_at']
        points = used - mark['used']
        size = sum(workload(counts))
        # Preserve sub-point work across unchanged readings. Record long plateaus as evidence too.
        useful = ((size >= 1 and (points > 0 or size >= 4)) or
                  (not any(counts.values()) and seconds >= 2 * 3600 and (points > 0 or seconds >= DAY)))
        if useful and used < 100:
            state['history'] = (state['history'] + [dict(counts, seconds=seconds, points=points)])[-HISTORY_LIMIT:]
            state['local'], state['shared'] = learn(state['history'], state['rate'])
        if (useful and used < 100) or used == 100:
            state['mark'] = dict(used=used, observed_at=observed_at, counts=state['counts'].copy())

    def calibrate_paired(self, observation):
        if not isinstance(observation, dict):
            raise ValueError('Missing paired reading')
        stamp, reset = observation.get('observed_at'), observation.get('reset')
        clock = time.time()
        if type(stamp) is not int or not 1735689600 <= stamp < 4102444800:
            raise ValueError('Invalid paired observation time')
        # On an unset RTC the nonce deadline on Core2 still fences the live reading.
        if clock >= 1735689600 and not -120 <= clock-stamp <= 600:
            raise ValueError('Paired reading is stale')
        if not isinstance(reset, str) or not re.fullmatch(r'[A-Z][a-z]{2} [1-9][0-9]?', reset):
            raise ValueError('Invalid reset date')
        datetime.strptime('2024 '+reset, '%Y %b %d')
        checked = datetime.fromtimestamp(stamp, timezone.utc)
        try:
            from zoneinfo import ZoneInfo
            checked = checked.astimezone(ZoneInfo('America/Los_Angeles'))
        except (ImportError, KeyError):
            pass
        short = f'{checked:%b} {checked.day} {checked:%H:%M}'
        label = f'{checked:%b} {checked.day}, {checked:%Y %H:%M %Z}'
        return self.calibrate(dict(observation, checked=short)), label

    def elapsed(self, anchor):
        if anchor.get('boot') == self.boot():
            if 'tick' in anchor:
                return max(0, time.monotonic() - anchor['tick'])
            elapsed = time.time() - anchor['received']
            return elapsed if elapsed >= 0 else None
        now = time.time()
        observed_at = anchor.get('observed_at', 0)
        return now - observed_at if 1735689600 <= observed_at <= now else None

    def estimate(self, state):
        anchor = state['anchor']
        if not anchor:
            return None
        delta = max(0, state['chars'] - anchor['chars'])
        local = state['local']
        elapsed = self.elapsed(anchor)
        if local and 'counts' in anchor:
            counts = {key:max(0, state['counts'][key] - anchor['counts'][key]) for key in COUNTERS}
            sent, reply = workload(counts)
            increase = local[0] * sent + local[1] * reply
        else:
            increase = delta * (state['rate'] or 0)
        if elapsed is not None:
            increase += min(elapsed / DAY, 1) * state['shared']
        learned = local is not None or bool(state['rate'])
        return dict(used=min(100, anchor['used'] + math.floor(increase + 0.5)), observed=anchor['used'],
                    reset=anchor['reset'], checked=anchor['checked'],
                    age=min(7 * DAY, int(elapsed)) if elapsed is not None else 7 * DAY,
                    learned=learned, pending=delta > 0 and not learned)

    def snapshot(self):
        with self.state(write=False) as state:
            result = self.estimate(state)
            if result:
                summary = self.accuracy_summary(state.get('accuracy', []))
                result['accuracy'] = dict(count=summary['scored'], latest_error_pp=summary['latest_error_pp'],
                                          recent_mae_tenths=round(10 * summary['recent']['mean_absolute_pp']) if summary['recent'] else None)
            return result


if __name__ == '__main__':
    import sys
    usage = Usage()
    if sys.argv[1] == '--accuracy':
        print(json.dumps(usage.accuracy_report(), indent=2))
    else:
        print(json.dumps(usage.calibrate(json.loads(Path(sys.argv[1]).read_text(encoding='utf8')))))
