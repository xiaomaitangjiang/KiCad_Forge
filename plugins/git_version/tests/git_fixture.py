"""Small throw-away Git repositories for the read-only graph tests."""
import datetime
import os
import subprocess
import sys
import tempfile

PLUGIN_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if PLUGIN_DIR not in sys.path:
    sys.path.insert(0, PLUGIN_DIR)

_BASE_TIME = datetime.datetime(2020, 1, 1, 3, 4, 5)


class GitFixture:
    """A temporary repository with deterministic commits and no global config."""

    def __init__(self):
        self._temp = tempfile.TemporaryDirectory(prefix="kf_git_")
        self.path = os.path.realpath(self._temp.name)
        self._tick = 0
        self.env = os.environ.copy()
        self.env.update({
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_TERMINAL_PROMPT": "0",
            "GIT_AUTHOR_NAME": "Test Author",
            "GIT_AUTHOR_EMAIL": "author@example.com",
            "GIT_COMMITTER_NAME": "Test Committer",
            "GIT_COMMITTER_EMAIL": "committer@example.com",
        })
        self.run("init", "-q")
        self.run("symbolic-ref", "HEAD", "refs/heads/main")
        self.run("config", "core.autocrlf", "false")
        self.run("config", "core.safecrlf", "false")
        self.run("config", "user.name", "Test Author")
        self.run("config", "user.email", "author@example.com")
        self.run("config", "commit.gpgsign", "false")
        self.run("config", "tag.gpgsign", "false")

    def close(self):
        self._temp.cleanup()

    def run(self, *args, input_text=None, env=None, check=True):
        environment = dict(self.env)
        if env:
            environment.update(env)
        completed = subprocess.run(
            ["git"] + list(args), cwd=self.path, input=input_text,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            encoding="utf-8", errors="replace", env=environment)
        if check and completed.returncode != 0:
            raise AssertionError(
                "git %r failed (%s)\nstdout: %s\nstderr: %s"
                % (args, completed.returncode, completed.stdout, completed.stderr))
        return completed

    def out(self, *args):
        return self.run(*args).stdout.strip()

    def write(self, relative_path, content):
        full = os.path.join(self.path, *relative_path.split("/"))
        os.makedirs(os.path.dirname(full), exist_ok=True) if os.path.dirname(full) else None
        if isinstance(content, bytes):
            with open(full, "wb") as handle:
                handle.write(content)
        else:
            with open(full, "w", encoding="utf-8", newline="") as handle:
                handle.write(content)

    def remove(self, relative_path):
        os.remove(os.path.join(self.path, *relative_path.split("/")))

    def stamp(self):
        self._tick += 1
        moment = _BASE_TIME + datetime.timedelta(seconds=60 * self._tick)
        return moment.strftime("%Y-%m-%dT%H:%M:%S+00:00")

    def commit(self, message, files=None, allow_empty=False,
               author_date=None, committer_date=None):
        if files:
            for relative_path, content in files.items():
                self.write(relative_path, content)
        self.run("add", "-A")
        stamp = self.stamp()
        env = {
            "GIT_AUTHOR_DATE": author_date or stamp,
            "GIT_COMMITTER_DATE": committer_date or stamp,
        }
        args = ["commit", "-q", "--no-gpg-sign"]
        if allow_empty:
            args.append("--allow-empty")
        args += ["-m", message]
        self.run(*args, env=env)
        return self.head()

    def head(self):
        return self.out("rev-parse", "HEAD")

    def git_value(self, oid, pretty):
        return self.out("log", "-1", "--format=" + pretty, oid)

    def status(self):
        return self.run("status", "--porcelain=v1").stdout

    def refs(self):
        return self.out("for-each-ref", "--format=%(refname) %(objectname)")
