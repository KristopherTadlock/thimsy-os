""".devcontainer/as-owner.sh, compose.yaml's entrypoint: it runs the command, hands its exit
status, stdin and signals through (SIGTERM, and SIGINT without a terminal, to the whole process
group), and gives what it created in the checkout to the checkout's owner. The ownership tests run
as root, as in the container (docker compose run --rm build tests/run.sh); the rest as any user on
Linux."""
import os
import signal
import subprocess
import sys
import time
import unittest

from tests import util

OWNER = os.path.join(util.ROOT, ".devcontainer", "as-owner.sh")
ROOT_ONLY = unittest.skipUnless(hasattr(os, "geteuid") and os.geteuid() == 0, "needs root (as in the container)")


def default_signal_env():
    """Whether env has --default-signal (GNU coreutils 8.31 or later), which as-owner.sh uses."""
    try:
        return subprocess.run(["env", "--default-signal=INT", "true"], capture_output=True).returncode == 0
    except OSError:
        return False


@unittest.skipUnless(sys.platform.startswith("linux") and default_signal_env(), "needs Linux and GNU env")
class AsOwner(util.TestCase):
    def setUp(self):
        super().setUp()
        self.co = self.path("checkout")
        os.makedirs(f"{self.co}/work")

    def owner(self, *cmd, **kw):
        return subprocess.run(["bash", OWNER, *cmd], cwd=self.co, capture_output=True, text=True, timeout=60, **kw)

    def test_exit_status(self):
        self.assertEqual(self.owner("bash", "-c", "exit 7").returncode, 7)

    def test_stdin_reaches_the_command(self):
        self.assertEqual(self.owner("cat", input="hi\n").stdout, "hi\n")

    def test_passes_signals_on(self):
        # Like build.sh: the command waits on a foreground program (sleep, as gradle or ninja) and has a
        # mod of its own running; both stop on docker stop's SIGTERM and on SIGINT without a terminal.
        mod = "trap 'echo mod >> mod_got; exit 3' TERM INT; sleep 30 & wait"
        child = f'env --default-signal=INT bash -c "{mod}" >/dev/null & echo ready; sleep 30; echo after'
        for sig, code in ((signal.SIGTERM, 143), (signal.SIGINT, 130)):
            # Its own session, as PID 1 in the container: kill 0 reaches its process group only.
            proc = subprocess.Popen(["bash", OWNER, "bash", "-c", child], cwd=self.co, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, text=True, start_new_session=True)
            try:
                self.assertEqual(proc.stdout.readline(), "ready\n")
                time.sleep(0.3)
                proc.send_signal(sig)
                out, _ = proc.communicate(timeout=20)
                time.sleep(0.3)
            finally:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            # The command stopped (it never said "after"), with the signal's status.
            self.assertEqual((proc.returncode, out), (code, ""), sig.name)
            got = os.path.join(self.co, "mod_got")
            self.assertEqual(util.slurp(got, text=True), "mod\n", sig.name)
            os.remove(got)

    @ROOT_ONLY
    def test_hands_back_what_it_made(self):
        os.chown(self.co, 1234, 1234)
        proc = self.owner("bash", "-c", "mkdir -p out/d; touch out/a out/d/b work/c; ln -s a out/l; exit 7")
        self.assertEqual(proc.returncode, 7, proc.stderr)
        for rel in ("out", "out/a", "out/d", "out/d/b"):
            self.assertEqual(os.stat(f"{self.co}/{rel}").st_uid, 1234, rel)
        self.assertEqual(os.lstat(f"{self.co}/out/l").st_uid, 1234)
        # work/ is the Docker volume: left to root.
        self.assertEqual(os.stat(f"{self.co}/work/c").st_uid, 0)

    @ROOT_ONLY
    def test_root_owned_checkout_is_left_alone(self):
        os.chown(self.co, 0, 0)
        # A root file in another group shows whether anything chowned it.
        open(f"{self.co}/x", "w").close()
        os.chown(f"{self.co}/x", 0, 1234)
        self.assertEqual(self.owner("touch", "y").returncode, 0)
        self.assertEqual((os.stat(f"{self.co}/x").st_uid, os.stat(f"{self.co}/x").st_gid), (0, 1234))
        self.assertEqual(os.stat(f"{self.co}/y").st_uid, 0)
