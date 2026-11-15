"""Bounded game logging shared by the Windows runner and Linux supervisor."""
import re


class LogLimit:
    """Keeps last_run.log small enough to send (a player's grew to about 100 MB). A line that keeps
    coming (the same words, its numbers aside) is written its first SAME times, then one in EVERY with
    its count; past MAX bytes, only the lines that say something went wrong; past twice that, nothing.
    The console and the performance report still get every line."""
    SAME, EVERY, MAX = 100, 1000, 10 << 20
    SHAPE = re.compile(r'0x[0-9a-fA-F]+|\d+')
    # what went wrong, not a count of it ("0 failed" in the renderer's statistics)
    CRITICAL = re.compile(r'(?i)\b(crash|fatal|exception|corrupt|bailing|abort|assert|unhandled)|'
                          r'(?<!\d )\b(error|errors|failed|fails)\b|^---')

    def __init__(self, written=0):
        self.seen = {}
        self.written = written  # an in-game restart appends to the same file

    def __call__(self, text):
        kept = []
        for line in text.splitlines(keepends=True):
            shape = self.SHAPE.sub('#', line[:160])
            count = self.seen.get(shape, 0) + 1
            if shape in self.seen or len(self.seen) < 50000:
                self.seen[shape] = count
            if count == self.SAME + 1:
                line = f'(log: the next line keeps coming; from here only one in {self.EVERY} is kept)\n' + line
            elif count > self.SAME:
                if count % self.EVERY:
                    continue
                line = f'(x{count}) {line}'
            if self.written >= 2 * self.MAX or (self.written >= self.MAX and not self.CRITICAL.search(line)):
                continue
            kept.append(line)
            self.written += len(line)
            if self.written >= self.MAX > self.written - len(line):
                kept.append(f'(log: over {self.MAX >> 20} MB; from here only the errors)\n')
        return ''.join(kept)

