#!/usr/bin/env python3
import argparse
from concurrent.futures import ProcessPoolExecutor, as_completed
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
from typing import List, NamedTuple, Optional, Tuple


# ANSI Color Codes
class Colors:
    HEADER = "\033[95m"
    BLUE = "\033[94m"
    CYAN = "\033[96m"
    GREEN = "\033[92m"
    YELLOW = "\033[93m"
    RED = "\033[91m"
    BOLD = "\033[1m"
    DIM = "\033[2m"
    RESET = "\033[0m"

    @classmethod
    def disable(cls):
        cls.HEADER = ""
        cls.BLUE = ""
        cls.CYAN = ""
        cls.GREEN = ""
        cls.YELLOW = ""
        cls.RED = ""
        cls.BOLD = ""
        cls.DIM = ""
        cls.RESET = ""


class FileResult(NamedTuple):
    file_path: Path
    rel_path: str
    status: str  # "MODIFIED", "UNCHANGED", "NEEDS_FORMAT", "ERROR"
    message: str = ""
    error_detail: str = ""


def format_single_file(
    file_path_str: str,
    root_dir_str: str,
    check_only: bool,
    style: str,
    clang_format_bin: str,
) -> FileResult:
    file_path = Path(file_path_str)
    root_dir = Path(root_dir_str)

    try:
        rel_path = str(file_path.relative_to(root_dir))
    except ValueError:
        rel_path = str(file_path)

    # Read original content
    try:
        with open(file_path, "rb") as f:
            original_content = f.read()
    except Exception as e:
        return FileResult(
            file_path=file_path,
            rel_path=rel_path,
            status="ERROR",
            error_detail=f"Failed to read file: {e}",
        )

    # Prepare clang-format command
    cmd = [clang_format_bin, f"--style={style}"]

    if check_only:
        # Dry run check
        cmd_check = cmd + ["--dry-run", "--Werror", str(file_path)]
        res = subprocess.run(cmd_check, capture_output=True, text=True)
        if res.returncode == 0:
            return FileResult(
                file_path=file_path, rel_path=rel_path, status="UNCHANGED"
            )
        else:
            return FileResult(
                file_path=file_path,
                rel_path=rel_path,
                status="NEEDS_FORMAT",
                message=res.stderr.strip() or res.stdout.strip(),
            )
    else:
        # Run clang-format to stdout to check diff before writing
        cmd_format = cmd + [str(file_path)]
        res = subprocess.run(cmd_format, capture_output=True)
        if res.returncode != 0:
            err_msg = res.stderr.decode("utf-8", errors="replace").strip()
            return FileResult(
                file_path=file_path,
                rel_path=rel_path,
                status="ERROR",
                error_detail=err_msg or "clang-format exited with error",
            )

        formatted_content = res.stdout

        if formatted_content == original_content:
            return FileResult(
                file_path=file_path, rel_path=rel_path, status="UNCHANGED"
            )
        else:
            try:
                with open(file_path, "wb") as f:
                    f.write(formatted_content)
                return FileResult(
                    file_path=file_path, rel_path=rel_path, status="MODIFIED"
                )
            except Exception as e:
                return FileResult(
                    file_path=file_path,
                    rel_path=rel_path,
                    status="ERROR",
                    error_detail=f"Failed to write file: {e}",
                )


def find_files(
    targets: List[Path], extensions: List[str], root_dir: Path
) -> List[Path]:
    files = []
    ext_set = {
        ext.lower() if ext.startswith(".") else f".{ext.lower()}"
        for ext in extensions
    }

    for target in targets:
        path = (
            target
            if target.is_absolute()
            else (root_dir / target).resolve()
        )
        if path.is_file():
            if path.suffix.lower() in ext_set:
                files.append(path)
        elif path.is_dir():
            for root, _, filenames in os.walk(path):
                for fname in filenames:
                    fpath = Path(root) / fname
                    if fpath.suffix.lower() in ext_set:
                        files.append(fpath)

    return sorted(list(set(files)))


def main():
    script_dir = Path(__file__).resolve().parent
    project_root = script_dir.parent

    default_extensions = [".cpp", ".hpp", ".c", ".h", ".cc", ".cxx", ".hh"]

    parser = argparse.ArgumentParser(
        description="Format all src/ files recursively in parallel"
    )
    parser.add_argument(
        "paths",
        nargs="*",
        default=["src"],
        help="Directories or files to format (default: src)",
    )
    parser.add_argument(
        "--check",
        "-c",
        action="store_true",
        help="Check formatting without modifying files (returns exit code 1 if unformatted)",
    )
    parser.add_argument(
        "-j",
        "--jobs",
        "--workers",
        type=int,
        default=os.cpu_count() or 4,
        help=f"Number of parallel workers (default: {os.cpu_count() or 4})",
    )
    parser.add_argument(
        "--style",
        type=str,
        default="file",
        help="clang-format style (default: file)",
    )
    parser.add_argument(
        "--ext",
        type=str,
        default=",".join(default_extensions),
        help=f"Comma-separated file extensions to format (default: {','.join(default_extensions)})",
    )
    parser.add_argument(
        "--clang-format-path",
        type=str,
        default="clang-format",
        help="Path to clang-format executable (default: clang-format)",
    )
    parser.add_argument(
        "--color",
        choices=["auto", "always", "never"],
        default="auto",
        help="Terminal color output (default: auto)",
    )
    parser.add_argument(
        "-q",
        "--quiet",
        action="store_true",
        help="Suppress per-file output, only print summary and errors",
    )

    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("the worker count must be at least 1")

    # Colors setup
    if args.color == "never" or (
        args.color == "auto" and not sys.stdout.isatty()
    ):
        Colors.disable()

    # Check clang-format executable
    clang_format_bin = shutil.which(args.clang_format_path) or args.clang_format_path
    try:
        version_res = subprocess.run(
            [clang_format_bin, "--version"], capture_output=True, text=True
        )
        if version_res.returncode != 0:
            raise RuntimeError("clang-format returned non-zero exit code")
        clang_version = version_res.stdout.strip().splitlines()[0]
    except Exception as e:
        print(
            f"{Colors.RED}{Colors.BOLD}Error:{Colors.RESET} Could not run clang-format binary '{args.clang_format_path}'",
            file=sys.stderr,
        )
        sys.exit(1)

    extensions = [e.strip() for e in args.ext.split(",") if e.strip()]
    target_paths = [Path(p) for p in args.paths]

    files_to_process = find_files(target_paths, extensions, project_root)

    # Terminal width logic
    term_width = min(shutil.get_terminal_size((80, 24)).columns, 100)
    divider = "-" * term_width

    mode_str = (
        f"{Colors.YELLOW}Check Mode (Dry-Run){Colors.RESET}"
        if args.check
        else f"{Colors.GREEN}Format Mode (In-Place){Colors.RESET}"
    )
    print(divider)
    print(f" {Colors.BOLD}Project Root {Colors.RESET}: {project_root}")
    print(
        f" {Colors.BOLD}Targets      {Colors.RESET}: {', '.join(args.paths)}"
    )
    print(
        f" {Colors.BOLD}Extensions   {Colors.RESET}: {', '.join(extensions)}"
    )
    print(
        f" {Colors.BOLD}Workers      {Colors.RESET}: {args.jobs} parallel threads/processes"
    )
    print(f" {Colors.BOLD}Formatter    {Colors.RESET}: {clang_version}")
    print(f" {Colors.BOLD}Total Files  {Colors.RESET}: {len(files_to_process)}")
    print(divider)

    if not files_to_process:
        print(
            f"{Colors.YELLOW}No matching source files found{Colors.RESET}"
        )
        print(divider)
        sys.exit(0)

    start_time = time.time()

    modified_count = 0
    unchanged_count = 0
    needs_format_count = 0
    error_count = 0
    errors: List[FileResult] = []

    total_files = len(files_to_process)
    padding = len(str(total_files))

    with ProcessPoolExecutor(max_workers=args.jobs) as executor:
        futures = {
            executor.submit(
                format_single_file,
                str(f),
                str(project_root),
                args.check,
                args.style,
                clang_format_bin,
            ): f
            for f in files_to_process
        }

        completed = 0
        for future in as_completed(futures):
            completed += 1
            file_path = futures[future]
            try:
                res: FileResult = future.result()
            except Exception as e:
                try:
                    rel_path = str(file_path.relative_to(project_root))
                except ValueError:
                    rel_path = str(file_path)
                res = FileResult(
                    file_path=file_path,
                    rel_path=rel_path,
                    status="ERROR",
                    error_detail=f"Formatter worker failed: {e}",
                )

            if res.status == "MODIFIED":
                modified_count += 1
                badge = f"{Colors.YELLOW}[  MODIFIED  ]{Colors.RESET}"
            elif res.status == "UNCHANGED":
                unchanged_count += 1
                badge = f"{Colors.GREEN}[ UNCHANGED  ]{Colors.RESET}"
            elif res.status == "NEEDS_FORMAT":
                needs_format_count += 1
                badge = f"{Colors.YELLOW}{Colors.BOLD}[NEEDS FORMAT]{Colors.RESET}"
            else:
                error_count += 1
                errors.append(res)
                badge = f"{Colors.RED}{Colors.BOLD}[   ERROR    ]{Colors.RESET}"

            if not args.quiet:
                progress = f"[{completed:{padding}d}/{total_files}]"
                print(
                    f" {Colors.CYAN}{progress}{Colors.RESET} {badge} {res.rel_path}"
                )

    elapsed_time = time.time() - start_time

    print(divider)
    print(f" {Colors.CYAN}{Colors.BOLD}SUMMARY STATISTICS{Colors.RESET}")
    print(divider)
    print(
        f" Total Files Processed : {Colors.BOLD}{total_files}{Colors.RESET}"
    )
    if args.check:
        print(
            f" Files Needing Format  : {Colors.YELLOW if needs_format_count > 0 else Colors.GREEN}{Colors.BOLD}{needs_format_count}{Colors.RESET}"
        )
    else:
        print(
            f" Files Formatted       : {Colors.YELLOW if modified_count > 0 else Colors.GREEN}{Colors.BOLD}{modified_count}{Colors.RESET}"
        )
    print(
        f" Files Unchanged       : {Colors.GREEN}{Colors.BOLD}{unchanged_count}{Colors.RESET}"
    )
    print(
        f" Errors Encountered    : {Colors.RED if error_count > 0 else Colors.GREEN}{Colors.BOLD}{error_count}{Colors.RESET}"
    )
    print(f" Time Elapsed          : {Colors.BOLD}{elapsed_time:.2f}s{Colors.RESET}")

    if errors:
        print(divider)
        print(f" {Colors.RED}{Colors.BOLD}ERRORS DETAILED:{Colors.RESET}")
        for err in errors:
            print(
                f"  - {Colors.BOLD}{err.rel_path}{Colors.RESET}: {err.error_detail}"
            )

    print(divider)

    if error_count > 0:
        sys.exit(1)
    elif args.check and needs_format_count > 0:
        sys.exit(1)
    else:
        sys.exit(0)


if __name__ == "__main__":
    main()
