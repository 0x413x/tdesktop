/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings.h"

bool gRtl = false;
Qt::LayoutDirection gLangDir = gRtl ? Qt::RightToLeft : Qt::LeftToRight;

bool gAlphaVersion = AppAlphaVersion;
uint64 gBetaVersion = AppBetaVersion;
uint64 gRealBetaVersion = AppBetaVersion;
QByteArray gBetaPrivateKey;

bool gTestMode = false;
bool gDebug = false;
bool gManyInstance = false;
QString gKeyFile;
QString gWorkingDir, gExeDir, gExeName;

QStringList gSendPaths;
QString gStartUrl;

QString gDialogLastPath, gDialogHelperPath; // optimize QFileDialog

bool gStartMinimized = false;
bool gStartInTray = false;
bool gAutoStart = false;
bool gSendToMenu = false;
bool gUseExternalVideoPlayer = false;
bool gAutoUpdate = true;
TWindowPos gWindowPos;
LaunchMode gLaunchMode = LaunchModeNormal;
bool gSupportTray = true;
bool gSeenTrayTooltip = false;
bool gRestartingUpdate = false, gRestarting = false, gRestartingToSettings = false, gWriteProtected = false;
int32 gLastUpdateCheck = 0;
bool gNoStartUpdate = false;
bool gStartToSettings = false;
bool gReplaceEmojis = true;

bool gCtrlEnter = false;

uint32 gConnectionsInSession = 1;
QString gLoggedPhoneNumber;

QByteArray gLocalSalt;
DBIScale gRealScale = dbisAuto;
DBIScale gScreenScale = dbisOne;
DBIScale gConfigScale = dbisAuto;

QString gTimeFormat = qsl("hh:mm");

RecentEmojiPack gRecentEmoji;
RecentEmojiPreload gRecentEmojiPreload;
EmojiColorVariants gEmojiVariants;

RecentStickerPreload gRecentStickersPreload;
RecentStickerPack gRecentStickers;

RecentHashtagPack gRecentWriteHashtags, gRecentSearchHashtags;

RecentInlineBots gRecentInlineBots;

bool gPasswordRecovered = false;
int32 gPasscodeBadTries = 0;
TimeMs gPasscodeLastTry = 0;

bool gRetina = false;
float64 gRetinaFactor = 1.;
int32 gIntRetinaFactor = 1;

#ifdef Q_OS_WIN
DBIPlatform gPlatform = dbipWindows;
#elif defined OS_MAC_OLD
DBIPlatform gPlatform = dbipMacOld;
#elif defined Q_OS_MAC
DBIPlatform gPlatform = dbipMac;
#elif defined Q_OS_LINUX64
DBIPlatform gPlatform = dbipLinux64;
#elif defined Q_OS_LINUX32
DBIPlatform gPlatform = dbipLinux32;
#else
#error Unknown platform
#endif
QString gPlatformString;
QUrl gUpdateURL;
bool gIsElCapitan = false;
bool gIsSnowLeopard = false;

int gOtherOnline = 0;

SavedPeers gSavedPeers;
SavedPeersByTime gSavedPeersByTime;

ReportSpamStatuses gReportSpamStatuses;

int32 gAutoDownloadPhoto = 0; // all auto download
int32 gAutoDownloadAudio = 0;
int32 gAutoDownloadGif = 0;
bool gAutoPlayGif = true;

bool gShowCallbackData = false;
bool gShowUsername = true;
bool gIgnoreBlocked = true;
bool gTagMention = true;
int gDialogsType = 0;
int gTyping = 0x131;
