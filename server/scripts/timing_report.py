"""Per-request server times from the log's "timing:" lines, as a table per request type.

docker logs game 2>&1 | python scripts/timing_report.py
python scripts/timing_report.py server.log [--top 30] [--sort p99|count|total]

GmeController::Handle logs one line per game request:
    timing: <handler> <micros> us <reply bytes> B [error]
The table gives, per handler, the count, p50, p95, p99 and max time and the total
time spent.  Run it on the same stretch of traffic before and after a change (for
example a day of prod logs) to see which calls got faster and whether the spikes
(p99, max) went down.
"""
import argparse
import re
import sys

LINE = re.compile(r'timing: (\S+) (\d+) us (\d+) B')


def percentile(sorted_values, fraction):
    """Nearest-rank percentile of an already sorted list."""
    index = max(0, min(len(sorted_values) - 1, round(fraction * len(sorted_values)) - 1))
    return sorted_values[index]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('log', nargs='?', help='log file (default: stdin)')
    parser.add_argument('--top', type=int, default=25, help='rows to print (default 25)')
    parser.add_argument('--sort', choices=('p99', 'count', 'total'), default='total',
                        help='order of the rows (default: total time)')
    args = parser.parse_args()

    source = open(args.log, encoding='utf-8', errors='replace') if args.log else sys.stdin
    times = {}
    with source:
        for line in source:
            match = LINE.search(line)
            if match:
                times.setdefault(match.group(1), []).append(int(match.group(2)))
    if not times:
        sys.exit('no "timing:" lines found')

    rows = []
    for handler, values in times.items():
        values.sort()
        rows.append((handler, len(values), percentile(values, 0.50), percentile(values, 0.95),
                     percentile(values, 0.99), values[-1], sum(values)))
    key = {'p99': lambda r: r[4], 'count': lambda r: r[1], 'total': lambda r: r[6]}[args.sort]
    rows.sort(key=key, reverse=True)

    everything = sorted(v for values in times.values() for v in values)
    print(f'{sum(len(v) for v in times.values())} requests, {len(times)} types; '
          f'all: p50 {percentile(everything, 0.5) / 1000:.1f} ms, '
          f'p99 {percentile(everything, 0.99) / 1000:.1f} ms, max {everything[-1] / 1000:.1f} ms')
    print(f'{"handler":28s} {"count":>7s} {"p50 ms":>8s} {"p95 ms":>8s} {"p99 ms":>8s} {"max ms":>8s} {"total s":>8s}')
    for handler, count, p50, p95, p99, top, total in rows[:args.top]:
        print(f'{handler[:28]:28s} {count:7d} {p50 / 1000:8.1f} {p95 / 1000:8.1f} '
              f'{p99 / 1000:8.1f} {top / 1000:8.1f} {total / 1e6:8.2f}')


if __name__ == '__main__':
    main()
