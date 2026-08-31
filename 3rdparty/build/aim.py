import os
import shutil
import argparse

FILES = ["AGENTS.md", "CLAUDE.md", "GEMINI.md"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "-d", "--delete",
        action="store_true",
        help="Delete generated AI instruction files"
    )
    args = parser.parse_args()

    if args.delete:
        for file in FILES:
            if os.path.exists(file):
                os.remove(file)
                print(f"Deleted: {file}")
    else:
        if not os.path.isfile("AI.md"):
            parser.error("AI.md not found")

        for file in FILES:
            shutil.copy("AI.md", file)
            print(f"Copied: AI.md -> {file}")


if __name__ == "__main__":
    main()

