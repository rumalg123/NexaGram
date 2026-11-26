'''
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
'''
import sys, os, re, io
from pathlib import Path

def finish(code):
    global executePath
    os.chdir(executePath)
    sys.exit(code)

executePath = os.getcwd()

if sys.platform == 'win32' and not 'COMSPEC' in os.environ:
    print('[ERROR] COMSPEC environment variable is not set.')
    finish(1)

scriptPath = os.path.dirname(os.path.realpath(__file__))

inputVersion = ''
versionOriginal = ''
versionMajor = ''
versionMinor = ''
versionPatch = ''
versionAlpha = '0'
versionBeta = False
if len(sys.argv) not in (3, 4):
  print("Usage: set_version.py <nexagram_version> <telegram_desktop_version> [64gram_version]")
  finish(1)

def lineageVersion(value):
  value = value.strip()
  if not re.fullmatch(r"\d+\.\d+(?:\.\d+)?(?:\.(?:\d+|beta))?", value, re.ASCII):
    print("Bad base version: " + value)
    finish(1)
  return value

versionUpstream = lineageVersion(sys.argv[2])
if len(sys.argv) == 4:
  versionBase = lineageVersion(sys.argv[3])
else:
  header = (Path(scriptPath) / "../SourceFiles/core/version.h").read_text(encoding="utf-8")
  match = re.search(r'Base64GramVersion\s*=\s*"([^"]+)"', header)
  if not match:
    print("Missing 64Gram base version")
    finish(1)
  versionBase = lineageVersion(match.group(1))

match = re.match(r'^\s*(\d+)\.(\d+)(\.(\d+)(\.(\d+|beta))?)?\s*$', sys.argv[1])
if match:
  inputVersion = sys.argv[1].strip()
  versionOriginal = inputVersion
  versionMajor = match.group(1)
  versionMinor = match.group(2)
  versionPatch = match.group(4) if match.group(4) else '0'
  versionAlphaBeta = match.group(5) if match.group(5) else ''
  if len(versionAlphaBeta) > 0:
    if match.group(6) == 'beta':
      versionBeta = True
    else:
      versionAlpha = match.group(6)

if not len(versionMajor):
  print("Wrong version parameter")
  finish(1)

def checkVersionPart(part):
  cleared = int(part) % 1000 if len(part) > 0 else 0
  if str(cleared) != part:
    print("Bad version part: " + part)
    finish(1)

checkVersionPart(versionMajor)
checkVersionPart(versionMinor)
checkVersionPart(versionPatch)
checkVersionPart(versionAlpha)

versionFull = str(int(versionMajor) * 1000000 + int(versionMinor) * 1000 + int(versionPatch))
versionFullAlpha = '0'
if versionAlpha != '0':
  versionFullAlpha = str(int(versionFull) * 1000 + int(versionAlpha))

versionStr = versionMajor + '.' + versionMinor + '.' + versionPatch
versionStrSmall = versionStr if versionPatch != '0' else versionMajor + '.' + versionMinor

if versionBeta:
  print('Setting version: ' + versionStr + ' beta')
elif versionAlpha != '0':
  print('Setting version: ' + versionStr + '.' + versionAlpha + ' closed alpha')
else:
  print('Setting version: ' + versionStr + ' stable')

#def replaceInFile(path, replaces):

def checkChangelog():
  global scriptPath, versionStr, versionStrSmall

  count = 0
  with io.open(scriptPath + '/../../changelog.txt', encoding='utf-8') as f:
    for line in f:
      if line.startswith(versionStr + ' ') or line.startswith(versionStrSmall + ' '):
        count = count + 1
  if count == 0:
    print('Changelog entry not found!')
    finish(1)
  elif count != 1:
    print('Wrong changelog entries count found: ' + count)
    finish(1)

#checkChangelog()

def replaceInFile(path, replacements):
  content = ''
  foundReplacements = {}
  updated = False
  with open(path, 'r', encoding='utf-8') as f:
    for line in f:
      for replacement in replacements:
        if re.search(replacement[0], line):
          changed = re.sub(replacement[0], replacement[1], line)
          if changed != line:
            line = changed
            updated = True
          foundReplacements[replacement[0]] = True
      content = content + line
  for replacement in replacements:
    if not replacement[0] in foundReplacements:
      print('Could not find "' + replacement[0] + '" in "' + path + '".')
      finish(1)
  if updated:
    with open(path, 'w', encoding='utf-8') as f:
      f.write(content)

print('Patching build/version...')
replaceInFile(scriptPath + '/version', [
  [ r'(AppVersion\s+)\d+', r'\g<1>' + versionFull ],
  [ r'(AppVersionStrMajor\s+)\d[\d\.]*', r'\g<1>' + versionMajor + '.' + versionMinor ],
  [ r'(AppVersionStrSmall\s+)\d[\d\.]*', r'\g<1>' + versionStrSmall ],
  [ r'(AppVersionStr\s+)\d[\d\.]*', r'\g<1>' + versionStr ],
  [ r'(UpstreamVersion\s+)\S+', r'\g<1>' + versionUpstream ],
  [ r'(Base64GramVersion\s+)\S+', r'\g<1>' + versionBase ],
  [ r'(BetaChannel\s+)\d', r'\g<1>' + ('1' if versionBeta else '0') ],
  [ r'(AlphaVersion\s+)\d+', r'\g<1>' + versionFullAlpha ],
  [ r'(AppVersionOriginal\s+)\d[\d\.beta]*', r'\g<1>' + versionOriginal ],
])

print('Patching core/version.h...')
replaceInFile(scriptPath + '/../SourceFiles/core/version.h', [
  [ r'(TDESKTOP_REQUESTED_ALPHA_VERSION\s+)\(\d+ULL\)', r'\g<1>(' + versionFullAlpha + 'ULL)' ],
  [ r'(AppVersion\s+=\s+)\d+', r'\g<1>' + versionFull ],
  [ r'(AppVersionStr\s+=\s+)[^;]+', r'\g<1>"' + versionStr + '"' ],
  [ r'(AppBetaVersion\s+=\s+)[a-z]+', r'\g<1>' + ('true' if versionBeta else 'false') ],
  [ r'(UpstreamVersion\s+=\s+)[^;]+', r'\g<1>"' + versionUpstream + '"' ],
  [ r'(Base64GramVersion\s+=\s+)[^;]+', r'\g<1>"' + versionBase + '"' ],
])

parts = [versionMajor, versionMinor, versionPatch, versionAlpha]
withcomma = ','.join(parts)
withdot = '.'.join(parts)
rcReplaces = [
  [ r'(FILEVERSION\s+)\d+,\d+,\d+,\d+', r'\g<1>' + withcomma ],
  [ r'(PRODUCTVERSION\s+)\d+,\d+,\d+,\d+', r'\g<1>' + withcomma ],
  [ r'("FileVersion",\s+)"\d+\.\d+\.\d+\.\d+"', r'\g<1>"' + withdot + '"' ],
  [ r'("ProductVersion",\s+)"\d+\.\d+\.\d+\.\d+"', r'\g<1>"' + withdot + '"' ],
]

print('Patching Telegram.rc...')
replaceInFile(scriptPath + '/../Resources/winrc/Telegram.rc', rcReplaces)

print('Patching Updater.rc...')
replaceInFile(scriptPath + '/../Resources/winrc/Updater.rc', rcReplaces)

print('Patching appxmanifest.xml...')
replaceInFile(scriptPath + '/../Resources/uwp/AppX/AppxManifest.xml', [
  [ r'( Version=)"\d+\.\d+\.\d+\.\d+"', r'\g<1>"' + withdot + '"' ],
])
