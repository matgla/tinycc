import sqlite3
import tempfile
import unittest
from pathlib import Path
from urllib.parse import parse_qs, urlparse

from metrics import codesize_detail_server


class CodesizeDetailServerTests(unittest.TestCase):
    def make_detail_db(self, path: Path) -> None:
        schema = Path(__file__).with_name("schema.sql").read_text()
        with sqlite3.connect(path) as conn:
            conn.executescript(schema)
            conn.execute(
                """INSERT INTO runs
                   (run_id, commit_sha, parent_sha, branch, subject, commit_ts,
                    run_ts, host)
                   VALUES (1, 'parent000000', NULL, 'test', 'parent', 1, 1, 'h')""")
            conn.execute(
                """INSERT INTO runs
                   (run_id, commit_sha, parent_sha, branch, subject, commit_ts,
                    run_ts, host)
                   VALUES (2, 'current00000', 'parent000000', 'test',
                           'current', 2, 2, 'h')""")
            rows = [
                (1, "suite", "file_reg.c", "reg", "o2", 10, 20, 0.5),
                (2, "suite", "file_reg.c", "reg", "o2", 12, 20, 0.6),
                (1, "suite", "file_imp.c", "imp", "o2", 20, 20, 1.0),
                (2, "suite", "file_imp.c", "imp", "o2", 15, 20, 0.75),
                (1, "suite", "file_same.c", "same", "o2", 7, 20, 0.35),
                (2, "suite", "file_same.c", "same", "o2", 7, 20, 0.35),
                (1, "suite", "file_mix.c", "grew", "o2", 10, 20, 0.5),
                (2, "suite", "file_mix.c", "grew", "o2", 12, 20, 0.6),
                (1, "suite", "file_mix.c", "shrunk", "o2", 30, 20, 1.5),
                (2, "suite", "file_mix.c", "shrunk", "o2", 25, 20, 1.25),
            ]
            conn.executemany(
                """INSERT INTO codesize_func
                   (run_id, suite, test, function, opt, tcc_size, gcc_size, ratio)
                   VALUES (?, ?, ?, ?, ?, ?, ?, ?)""",
                rows)

    def render_detail_page(self, db: Path, path: str) -> str:
        class TestHandler(codesize_detail_server.CodesizeHandler):
            db_path = str(db)

            def send_html(self, title: str, body: str, status: int = 200) -> None:
                self.title = title
                self.body = body
                self.status = status

        handler = object.__new__(TestHandler)
        parsed = urlparse(path)
        query = parse_qs(parsed.query)
        if parsed.path == "/compare":
            handler.handle_compare(query)
        elif parsed.path == "/file":
            handler.handle_file(query)
        else:
            raise AssertionError(f"unsupported test path: {path}")
        self.assertEqual(handler.status, 200)
        return handler.body

    def test_missing_detail_db_reports_path_and_setup_hint(self):
        with tempfile.TemporaryDirectory() as tmp:
            db = Path(tmp) / "missing.db"

            with self.assertRaises(codesize_detail_server.DatabaseOpenError) as exc:
                codesize_detail_server.connect(str(db))

        message = str(exc.exception)
        self.assertIn(f"detail database does not exist: {db}", message)
        self.assertIn("sqlite3", message)
        self.assertIn("metrics/schema.sql", message)

    def test_connect_readonly_escapes_uri_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            db = Path(tmp) / "detail db with spaces.db"
            with sqlite3.connect(db):
                pass

            with codesize_detail_server.connect(str(db)) as conn:
                row = conn.execute("SELECT 1 AS value").fetchone()

        self.assertEqual(row["value"], 1)

    def test_compare_sorts_by_requested_column(self):
        with tempfile.TemporaryDirectory() as tmp:
            db = Path(tmp) / "detail.db"
            self.make_detail_db(db)

            page = self.render_detail_page(
                db, "/compare?run=2&opt=o2&sort=tcc&dir=asc")

        self.assertLess(page.index("<td>file_same.c</td>"),
                        page.index("<td>file_reg.c</td>"))
        self.assertLess(page.index("<td>file_reg.c</td>"),
                        page.index("<td>file_imp.c</td>"))

    def test_compare_filters_regressions_and_improvements(self):
        with tempfile.TemporaryDirectory() as tmp:
            db = Path(tmp) / "detail.db"
            self.make_detail_db(db)

            regressions = self.render_detail_page(
                db, "/compare?run=2&opt=o2&change=regressions")
            improvements = self.render_detail_page(
                db, "/compare?run=2&opt=o2&change=improvements")

        self.assertIn("<td>file_reg.c</td>", regressions)
        self.assertNotIn("<td>file_imp.c</td>", regressions)
        self.assertIn("<td>file_imp.c</td>", improvements)
        self.assertNotIn("<td>file_reg.c</td>", improvements)

    def test_file_filters_regressed_functions(self):
        with tempfile.TemporaryDirectory() as tmp:
            db = Path(tmp) / "detail.db"
            self.make_detail_db(db)

            page = self.render_detail_page(
                db,
                "/file?run=2&opt=o2&suite=suite&test=file_mix.c"
                "&change=regressions")

        self.assertIn("<td>grew</td>", page)
        self.assertNotIn("<td>shrunk</td>", page)

    def test_file_sorts_by_requested_column(self):
        with tempfile.TemporaryDirectory() as tmp:
            db = Path(tmp) / "detail.db"
            self.make_detail_db(db)

            page = self.render_detail_page(
                db,
                "/file?run=2&opt=o2&suite=suite&test=file_mix.c"
                "&sort=tcc&dir=asc")

        self.assertLess(page.index("<td>grew</td>"),
                        page.index("<td>shrunk</td>"))


if __name__ == "__main__":
    unittest.main()
