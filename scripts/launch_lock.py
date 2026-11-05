"""One launch per writable data directory, including preparation and in-game restarts."""
from contextlib import contextmanager
import os
from pathlib import Path
import sys
import subprocess
import signal
from mod_components import long_path

HELD = {}
FD_ENV = 'BB_LAUNCH_LOCK_FD'


def lock_file(stream):
    stream.seek(0)
    if os.name == 'nt':
        import msvcrt
        msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
    else:
        import fcntl
        fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)


def inherited(path):
    """Linux run.sh inherits the supervisor's open lock description."""
    if os.name == 'nt':
        return False
    try:
        fd = int(os.environ.get(FD_ENV, '-1'))
        if fd < 3 or not os.path.samestat(os.fstat(fd), path.stat()):
            return False
        import fcntl
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except (ValueError, OSError):
        return False


def lock_path(data):
    return long_path((Path(data) / 'out').resolve()) / 'launch.lock'


@contextmanager
def session(data):
    with output_session(Path(data) / 'out'):
        yield


@contextmanager
def output_session(out):
    # Resolve the output directory too: an out/ symlink must not create a second lock identity.
    path = long_path(Path(out).resolve()) / 'launch.lock'
    if path in HELD or inherited(path):
        yield
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_symlink() or (hasattr(path, 'is_junction') and path.is_junction()):
        raise ValueError('Launch lock must not be a symlink or junction: ' + str(path))
    with path.open('a+b') as stream:
        # Windows byte-range locks require a byte even on a new installation.
        if path.stat().st_size == 0:
            stream.write(b'0')
            stream.flush()
        try:
            lock_file(stream)
        except OSError as error:
            raise RuntimeError('Another game launch is using this data directory: ' + str(path.parent.parent)) from error
        HELD[path] = stream
        try:
            yield
        finally:
            del HELD[path]
            # Closing releases the OS lock, including on exceptions. Never unlink its inode.


def main():
    mode, data, *command = sys.argv[1:]
    if mode == '--check':
        return 0 if inherited(lock_path(data)) else 1
    if mode != '--run' or not command or os.name == 'nt':
        raise ValueError('usage: launch_lock.py --run DATA COMMAND [ARGS...] (POSIX)')
    with session(data):
        stream = HELD[lock_path(data)]
        os.environ[FD_ENV] = str(stream.fileno())
        # The runtime closes descriptors on exec-based restart. Exit to this supervisor instead,
        # preserving the lock while each launch refreshes settings and other dynamic output.
        os.environ['BB_RESTART_EXIT_CODE'] = '75'
        os.environ.pop('BB_RESTART_COMMAND', None)
        while True:
            child = None
            received = [0]
            def stop(signum, _frame):
                received[0] = signum
                if child is not None:
                    raise KeyboardInterrupt
            signals = (signal.SIGTERM, signal.SIGINT, signal.SIGHUP)
            previous = {sig: signal.signal(sig, stop) for sig in signals}
            try:
                with subprocess.Popen(command, pass_fds=(stream.fileno(),), start_new_session=True) as child:
                    try:
                        if received[0]:
                            raise KeyboardInterrupt
                        code = child.wait()
                    except KeyboardInterrupt:
                        # Steam may signal only the supervisor. Stop its whole child group,
                        # including the logging pipeline's runtime, before releasing the lock.
                        for sig in signals:
                            signal.signal(sig, signal.SIG_IGN)
                        try:
                            os.killpg(child.pid, signal.SIGTERM)
                        except ProcessLookupError:
                            pass
                        try:
                            child.wait(timeout=2)
                        except subprocess.TimeoutExpired:
                            pass
                        try:
                            os.killpg(child.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                        child.wait()
                        return 128 + (received[0] or signal.SIGINT)
            finally:
                for sig, handler in previous.items():
                    signal.signal(sig, handler)
            if code != 75:
                return code if code >= 0 else 128 - code


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError) as error:
        print('Launch blocked: ' + str(error), file=sys.stderr)
        sys.exit(2)
