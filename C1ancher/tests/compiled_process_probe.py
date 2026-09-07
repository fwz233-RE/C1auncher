"""Load actual Windows-compiled probe bytes without Python newline normalization."""
from pathlib import Path
import subprocess

DOTNET = '/mnt/c/Program Files/dotnet/dotnet.exe'
ASSEMBLY = r'D:\c1slim\C1ancher\installer\tests\bin\Release\net8.0\Installer.OfflineTests.dll'


def compiled_probe():
    if not Path(DOTNET).is_file():
        raise AssertionError('Build Windows offline tests and run this regression in WSL')
    result = subprocess.run([DOTNET, ASSEMBLY, '--print-running-process-command'],
                            capture_output=True, check=True, timeout=30)
    # Never text=True/read_text(): either would hide CRLF embedded in the EXE.
    command = result.stdout.decode('utf-8')
    if '\r' in command or '\x00' in command or '; do\n' not in command:
        raise AssertionError('Compiled process probe is not a canonical LF shell command')
    return command
