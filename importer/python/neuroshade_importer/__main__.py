"""Allow ``python3 -m neuroshade_importer ...`` as an alternative entry point."""

from .cli import main

if __name__ == "__main__":
    raise SystemExit(main())
