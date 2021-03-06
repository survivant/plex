/*
    This file is part of libscrobbler. Modified for XBMC by Bobbin007

    libscrobbler is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    libscrobbler is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with libscrobbler; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

  Copyright � 2003 Russell Garrett (russ-scrobbler@garrett.co.uk)
*/
#include "stdafx.h"
#include "scrobbler.h"
#include "errors.h"
#include "utils/md5.h"
#include "utils/CharsetConverter.h"
#include "utils/log.h"
#include "utils/HTTP.h"
#include "Util.h"
#include "Settings.h"
#include "Application.h"
#include "MusicInfoTag.h"
#include "FileSystem/File.h"
#include "tinyXML/tinyxml.h"
#include "XMLUtils.h"
#include "RegExp.h"

using namespace std;
using namespace MUSIC_INFO;
using namespace XFILE;

#define LS_VER    "1.4"
#define MINLENGTH 30
#define MAXLENGTH 10800
#define HS_FAIL_WAIT 100000
#define MAXSUBMISSIONS 50

//#define CLIENT_ID "tst"
#define CLIENT_ID "pmc"
#define CLIENT_VERSION "0.1"

CScrobbler* CScrobbler::m_pInstance=NULL;

CScrobbler::CScrobbler()
{
  m_bShouldSubmit=false;
  m_bUpdateWarningDone=false;
  m_bConnectionWarningDone=false;
  m_bAuthWarningDone=false;
  m_bBadPassWarningDone=false;
  m_bReHandShaking=false;
  m_strClientId = CLIENT_ID;
  m_strClientVer = CLIENT_VERSION;
  m_bCloseThread = false;
  m_Interval = m_LastConnect = m_SongStartTime = 0;
  m_iSecsTillSubmit=0;
  m_bSubmitInProgress = false;
  m_bReadyToSubmit=false;
  m_iSongNum=0;
  DWORD threadid; // Needed for Win9x
  m_hWorkerEvent = CreateEvent(NULL, false, false, NULL);
  if (!m_hWorkerEvent)
    throw EOutOfMemory();
  m_hWorkerThread = CreateThread(NULL, 0, threadProc, (LPVOID)this, 0, &threadid);
  if (!m_hWorkerThread)
    throw EOutOfMemory();
}

CScrobbler::~CScrobbler()
{
  m_bCloseThread = true;
  SetEvent(m_hWorkerEvent);
  WaitForSingleObject(m_hWorkerThread, INFINITE);
  CloseHandle(m_hWorkerEvent);
  CloseHandle(m_hWorkerThread);
  Sleep(0);
}

CScrobbler* CScrobbler::GetInstance()
{
  if (!m_pInstance)
    m_pInstance=new CScrobbler;

  return m_pInstance;
}

void CScrobbler::RemoveInstance()
{
  if (m_pInstance)
  {
    m_pInstance->Term();
    delete m_pInstance;
    m_pInstance=NULL;
  }
}

void CScrobbler::Init()
{
  if ((!g_guiSettings.GetBool("lastfm.enable") && !g_guiSettings.GetBool("lastfm.recordtoprofile")) || !g_guiSettings.GetBool("network.enableinternet"))
    return;

  CStdString strPassword=g_guiSettings.GetString("lastfm.password");
  CStdString strUserName=g_guiSettings.GetString("lastfm.username");

  if (strPassword.IsEmpty() || strUserName.IsEmpty())
    return;

  // show all warnings each time we init the scrobbler
  m_bAuthWarningDone=false;
  m_bUpdateWarningDone=false;
  m_bConnectionWarningDone=false;
  m_bBadPassWarningDone=false;

  SetPassword(strPassword);
  SetUsername(strUserName);

  LoadJournal();
  DoHandshake();
}

void CScrobbler::Term()
{
  m_bReadyToSubmit=false;
  SaveJournal();
}

void CScrobbler::SetPassword(const CStdString& strPass)
{
  if (strPass.IsEmpty())
    return;
  XBMC::MD5 md5state;
  unsigned char md5pword[16];
  md5state.append(strPass);
  md5state.getDigest(md5pword);
  char tmp[33];
  strncpy(tmp, "\0", sizeof(tmp));
  for (int j = 0;j < 16;j++)
  {
    char a[3];
    sprintf(a, "%02x", md5pword[j]);
    tmp[2*j] = a[0];
    tmp[2*j+1] = a[1];
  }
  m_strPassword = tmp;
  GenSessionKey();
}

void CScrobbler::SetUsername(const CStdString& strUser)
{
  if (strUser.IsEmpty())
    return;

  m_strUserName=strUser;

  CUtil::URLEncode(m_strUserName);

  m_strHsString = "http://post.audioscrobbler.com/?hs=true&p=1.1&c=" + m_strClientId + "&v=" + m_strClientVer + "&u=" + m_strUserName;
}

int CScrobbler::AddSong(const CMusicInfoTag& tag)
{
  if ((!g_guiSettings.GetBool("lastfm.enable") && !g_guiSettings.GetBool("lastfm.recordtoprofile")) || !g_guiSettings.GetBool("network.enableinternet"))
    return 0;

  if (tag.GetDuration() <= MINLENGTH || tag.GetDuration() > MAXLENGTH) // made <= to minlength to stop iTMS previews being submitted in iTunes
    return 0;

  if (!tag.Loaded() || tag.GetArtist().IsEmpty() || tag.GetTitle().IsEmpty())
    return 0;

  if(m_bSubmitInProgress)
  {
    StatusUpdate(S_NOT_SUBMITTING,"Previous submission still in progress");
    return 0;
  }

  char ti[20];
  struct tm *today = gmtime(&m_SongStartTime);
  strftime(ti, sizeof(ti), "%Y-%m-%d %H:%M:%S", today);

  SubmissionJournalEntry entry;
  // our tags are stored as UTF-8, so no conversion needed
  entry.strArtist = tag.GetArtist();
  entry.strAlbum = tag.GetAlbum();
  entry.strTitle = tag.GetTitle();
  entry.strStartTime = ti;
  entry.strLength.Format("%d",tag.GetDuration());
  entry.strMusicBrainzID = tag.GetMusicBrainzTrackID();
  m_vecSubmissionJournal.push_back(entry);
  SaveJournal();

  while ((unsigned)m_iSongNum < m_vecSubmissionJournal.size() && m_iSongNum < MAXSUBMISSIONS)
  {
    entry = m_vecSubmissionJournal[m_iSongNum];
    CStdString strStartTime = entry.strStartTime;
    CUtil::URLEncode(entry.strArtist); 
    CUtil::URLEncode(entry.strTitle);
    CUtil::URLEncode(entry.strAlbum);
    CUtil::URLEncode(entry.strMusicBrainzID);
    CUtil::URLEncode(entry.strLength);
    CUtil::URLEncode(entry.strStartTime);
    CStdString strSubmitStr;
    strSubmitStr.Format("a[%i]=%s&t[%i]=%s&b[%i]=%s&m[%i]=%s&l[%i]=%s&i[%i]=%s&", 
        m_iSongNum, 
        entry.strArtist, 
        m_iSongNum, 
        entry.strTitle,
        m_iSongNum,
        entry.strAlbum,
        m_iSongNum, 
        entry.strMusicBrainzID,
        m_iSongNum, 
        entry.strLength,
        m_iSongNum, 
        entry.strStartTime);

    if(m_strPostString.find(strStartTime) != m_strPostString.npos)
    {
      // we have already tried to add a song at this time stamp
      // I have no idea how this could happen but apparently it does so
      // we stop it now

      StatusUpdate(S_NOT_SUBMITTING,strSubmitStr);
      StatusUpdate(S_NOT_SUBMITTING,m_strPostString);
      StatusUpdate(S_NOT_SUBMITTING,"Submission error, duplicate subbmission time found");
      
      m_vecSubmissionJournal.erase(m_vecSubmissionJournal.begin() + m_iSongNum);
      SaveJournal();

      return 3;
    }

    m_strPostString += strSubmitStr;
    m_iSongNum++;
  }

  time_t now;
  time (&now);
  if ((m_Interval + m_LastConnect) < now)
  {
    DoSubmit();
    return 1;
  }
  else
  {
    CStdString strMsg;
    strMsg.Format("Not submitting, caching for %i more seconds. Cache is %i entries.", (int)(m_Interval + m_LastConnect - now), m_vecSubmissionJournal.size());
    StatusUpdate(S_NOT_SUBMITTING,strMsg);
    return 2;
  }
}

void CScrobbler::DoHandshake()
{
  m_bReadyToSubmit = false;
  time_t now;
  time (&now);
  m_LastConnect = now;
  m_strHsString = "http://post.audioscrobbler.com/?hs=true&p=1.1&c=" + m_strClientId + "&v=" + m_strClientVer + "&u=" + m_strUserName;
  SetEvent(m_hWorkerEvent);
}

void CScrobbler::DoSubmit()
{
  if(m_bSubmitInProgress)
  {
    StatusUpdate(S_NOT_SUBMITTING,"Previous submission still in progress");
    return;
  }

  if (m_strUserName.IsEmpty() || m_strPassword.IsEmpty() || !m_bReadyToSubmit)
    return;

  m_bSubmitInProgress = true;

  StatusUpdate(S_SUBMITTING,"Submitting cache...");
  time_t now;
  time (&now);
  m_LastConnect = now;
  m_strSubmit.Format("u=%s&s=%s&", m_strUserName.c_str(), m_strSessionKey.c_str());
  m_strSubmit += m_strPostString;
  StatusUpdate(S_SUBMITTING,m_strSubmit);
  SetEvent(m_hWorkerEvent);
}

void CScrobbler::HandleHandshake(char *handshake)
{
  // Doesn't take into account multiple-packet returns (not that I've seen one yet)...

  // Ian says: strtok() is not re-entrant, but since it's only being called
  //  in only one function at a time, it's ok so far.

  char seps[] = " \n\r";
  char *response = strtok(handshake, seps);
  if (!response)
  {
    StatusUpdate(S_HANDSHAKE_INVALID_RESPONSE,"Handshake failed: Response invalid");
    return;
  }
  do
  {
    if (stricmp("UPTODATE", response) == 0)
    {
      StatusUpdate(S_HANDSHAKE_UP_TO_DATE,"Handshaking: Client up to date.");
    }
    else if (stricmp("UPDATE", response) == 0)
    {
      char *updateurl = strtok(NULL, seps);
      if (!updateurl)
        break;
      string msg = "Handshaking: Please update your client at: ";
      msg += updateurl;
      StatusUpdate(S_HANDSHAKE_OLD_CLIENT,msg.c_str());
    }
    else if (stricmp("BADUSER", response) == 0)
    {
      StatusUpdate(S_HANDSHAKE_BAD_USERNAME,"Handshake failed: Bad username");
      return;
    }
    else
    {
      break;
    }
    m_strChallenge = strtok(NULL, seps);
    m_strSubmitUrl = strtok(NULL, seps);
    char *inttext = strtok(NULL, seps);
    if (!inttext || !(stricmp("INTERVAL", inttext) == 0))
      break;
    SetInterval(atoi(strtok(NULL, seps)));
    GenSessionKey();
    m_bReadyToSubmit = true;
    StatusUpdate( S_HANDSHAKE_SUCCESS,"Handshake successful.");
    return;
  } while (0);
  CStdString strBuf;
  strBuf.Format("Handshake failed: %s", strtok(NULL, "\n"));
  StatusUpdate(S_HANDSHAKE_ERROR, strBuf);
}

void CScrobbler::HandleSubmit(char *data)
{
  //  submit returned
  m_bSubmitInProgress = false; //- this should already have been cancelled by the header callback

  StatusUpdate(S_DEBUG,data);

  // Doesn't take into account multiple-packet returns (not that I've seen one yet)...
  char seps[] = " \n\r";
  char * response = strtok(data, seps);
  if (!response)
  {
    StatusUpdate(S_SUBMIT_INVALID_RESPONSE,"Submission failed: Response invalid");
    return;
  }
  if (stricmp("OK", response) == 0)
  {
    StatusUpdate(S_SUBMIT_SUCCESS,"Submission succeeded.");

    char *inttext = strtok(NULL, seps);
    if (inttext && (stricmp("INTERVAL", inttext) == 0))
    {
      m_Interval = atoi(strtok(NULL, seps));
      CStdString strBuf;
      strBuf.Format("Submit interval set to %i seconds.", m_Interval);
      StatusUpdate(S_SUBMIT_INTERVAL, strBuf);
    }
    
    // Remove successfully submitted songs from journal and clear POST data
    std::vector<SubmissionJournalEntry>::iterator it;
    it = m_vecSubmissionJournal.begin();
    m_vecSubmissionJournal.erase(it, it + m_iSongNum);
    m_strPostString = "";
    m_iSongNum = 0;
    SaveJournal();
  }
  else if (stricmp("BADPASS", response) == 0)
  {
    StatusUpdate(S_SUBMIT_BAD_PASSWORD,"Submission failed: bad password.");
  }
  else if (stricmp("FAILED", response) == 0)
  {
    CStdString strBuf;
    strBuf.Format("Submission failed: %s", strtok(NULL, "\n"));
    StatusUpdate(S_SUBMIT_FAILED, strBuf);
    char *inttext = strtok(NULL, seps);
    if (inttext && (stricmp("INTERVAL", inttext) == 0))
    {
      m_Interval = atoi(strtok(NULL, seps));
      strBuf.Format("Submit interval set to %i seconds.", m_Interval);
      StatusUpdate(S_SUBMIT_INTERVAL, strBuf);
    }
  }
  else if (stricmp("BADAUTH",response) == 0)
  {
    StatusUpdate(S_SUBMIT_BADAUTH,"Submission failed: bad authorization.");
    char *inttext = strtok(NULL, seps);
    if (inttext && (stricmp("INTERVAL", inttext) == 0))
    {
      m_Interval = atoi(strtok(NULL, seps));
      CStdString strBuf;
      strBuf.Format("Submit interval set to %i seconds.", m_Interval);
      StatusUpdate(S_SUBMIT_INTERVAL, strBuf);
    }
    //re-handshake
    m_bReHandShaking = true;
    DoHandshake();
  }
  else
  {
    CStdString strBuf;
    strBuf.Format("Submission failed: %s", strtok(NULL, "\n"));
    StatusUpdate(S_SUBMIT_FAILED, strBuf);
  }
}

void CScrobbler::SetInterval(int in)
{
  m_Interval = in;
  CStdString ret;
  ret.Format("Submit interval set to %d seconds.", in);
  StatusUpdate(S_SUBMIT_INTERVAL, ret);
}

void CScrobbler::GenSessionKey()
{
  CStdString clear = m_strPassword + m_strChallenge;
  unsigned char md5pword[16];
  XBMC::MD5 md5state;
  md5state.append(clear);
  md5state.getDigest(md5pword);

  char key[33];
  strncpy(key, "\0", sizeof(key));
  for (int j = 0;j < 16;j++)
  {
    char a[3];
    sprintf(a, "%02x", md5pword[j]);
    key[2*j] = a[0];
    key[2*j+1] = a[1];
  }
  m_strSessionKey = key;
}

int CScrobbler::LoadCache()
{
  CFile file;
  if (file.Open(GetTempFileName()))
  {
    CArchive ar(&file, CArchive::load);
    ar >> m_iSongNum;
    ar >> m_strPostString;
    ar.Close();
    file.Close();
    return 1;
  }
  return 0;
}

int CScrobbler::LoadJournal()
{
  SubmissionJournalEntry entry;
  m_vecSubmissionJournal.clear();
  
  if (LoadCache()) 
  {
    // load old style cache
    CLog::Log(LOGDEBUG, "Audioscrobbler: Found old submission cache, converting to submission journal.");
    for (int i = 0; i < m_iSongNum; i++)
    {
      CStdString strRegEx;
      strRegEx.Format("a\\[%i\\]=(.*?)&t\\[%i\\]=(.*?)&b\\[%i\\]=(.*?)&m\\[%i\\]=(.*?)&l\\[%i\\]=(.*?)&i\\[%i\\]=(.*?)&", i, i, i, i, i, i);
      CRegExp re;
      if (!re.RegComp(strRegEx.c_str()))
        break;
      if (re.RegFind(m_strPostString) < 0)
        continue;
      
      char *s;
      s = re.GetReplaceString("\\1"); 
      entry.strArtist = s;
      free(s);
      s = re.GetReplaceString("\\2"); 
      entry.strTitle = s;
      free(s);
      s = re.GetReplaceString("\\3"); 
      entry.strAlbum = s;
      free(s);
      s = re.GetReplaceString("\\4"); 
      entry.strMusicBrainzID = s;
      free(s);
      s = re.GetReplaceString("\\5"); 
      entry.strLength = s;
      free(s);
      s = re.GetReplaceString("\\6"); 
      entry.strStartTime = s;
      free(s);
      CUtil::UrlDecode(entry.strArtist);
      CUtil::UrlDecode(entry.strTitle);
      CUtil::UrlDecode(entry.strAlbum);
      CUtil::UrlDecode(entry.strMusicBrainzID);
      CUtil::UrlDecode(entry.strLength);
      CUtil::UrlDecode(entry.strStartTime);
      m_vecSubmissionJournal.push_back(entry);
    }
    m_iSongNum = 0;
    m_strPostString = "";
    ::DeleteFile(GetTempFileName());
    CLog::Log(LOGDEBUG, "Audioscrobbler: Added %d entries from old cache file (%s) to journal.", m_vecSubmissionJournal.size(), GetTempFileName().c_str());
  }

  TiXmlDocument xmlDoc;
  CStdString JournalFileName = GetJournalFileName();
  if (!xmlDoc.LoadFile(JournalFileName))
  {
    CLog::Log(LOGDEBUG, "Audioscrobbler: %s, Line %d (%s)", JournalFileName.c_str(), xmlDoc.ErrorRow(), xmlDoc.ErrorDesc());
    return 0;
  }

  TiXmlElement *pRoot = xmlDoc.RootElement();
  if (strcmpi(pRoot->Value(), "asjournal") != 0)
  {
    CLog::Log(LOGDEBUG, "Audioscrobbler: %s missing <asjournal>", JournalFileName.c_str());
    return 0;
  }

  TiXmlNode *pNode = pRoot->FirstChild("entry");
  while (pNode)
  {
    XMLUtils::GetString(pNode, "artist", entry.strArtist);
    XMLUtils::GetString(pNode, "album", entry.strAlbum);
    XMLUtils::GetString(pNode, "title", entry.strTitle);
    XMLUtils::GetString(pNode, "length", entry.strLength);
    XMLUtils::GetString(pNode, "starttime", entry.strStartTime);
    XMLUtils::GetString(pNode, "musicbrainzid", entry.strMusicBrainzID);
    m_vecSubmissionJournal.push_back(entry);
    pNode = pNode->NextSibling("entry");
  }

  CLog::Log(LOGDEBUG, "Audioscrobbler: Journal loaded with %d entries.", m_vecSubmissionJournal.size());
  return (m_vecSubmissionJournal.size() > 0) ? 1 : 0;
}

int CScrobbler::SaveJournal()
{
  TiXmlDocument xmlDoc;
  TiXmlDeclaration decl("1.0", "utf-8", "yes");
  xmlDoc.InsertEndChild(decl);
  TiXmlElement xmlRootElement("asjournal");
  TiXmlNode *pRoot = xmlDoc.InsertEndChild(xmlRootElement);
  if (!pRoot)
    return 0;
  std::vector<SubmissionJournalEntry>::iterator it = m_vecSubmissionJournal.begin();
  for (; it != m_vecSubmissionJournal.end(); it++)
  {
    TiXmlElement entryNode("entry");
    TiXmlNode *pNode = pRoot->InsertEndChild(entryNode);
    if (!pNode)
      return 0;
    XMLUtils::SetString(pNode, "artist", it->strArtist);
    XMLUtils::SetString(pNode, "album", it->strAlbum);
    XMLUtils::SetString(pNode, "title", it->strTitle);
    XMLUtils::SetString(pNode, "length", it->strLength);
    XMLUtils::SetString(pNode, "starttime", it->strStartTime);
    XMLUtils::SetString(pNode, "musicbrainzid", it->strMusicBrainzID);
  }

  CStdString FileName = GetJournalFileName();
  return (xmlDoc.SaveFile(FileName)) ? 1 : 0;
}

void CScrobbler::StatusUpdate(ScrobbleStatus status, const CStdString& strText)
{
  switch (status)
  {
    case S_HANDSHAKE_ERROR:
    case S_SUBMIT_FAILED:
    case S_HANDSHAKE_INVALID_RESPONSE:
    case S_SUBMIT_INVALID_RESPONSE:
    case S_CONNECT_ERROR:
      // these are the bad ones just log
      CLog::Log(LOGERROR, "AudioScrobbler: %s", strText.c_str());
      break;
    case S_HANDSHAKE_SUCCESS:
      CLog::Log(LOGNOTICE, "AudioScrobbler: %s", strText.c_str());
      break;
    case S_HANDSHAKE_BAD_USERNAME:
    case S_SUBMIT_BADAUTH:
      CLog::Log(LOGERROR, "AudioScrobbler: %s", strText.c_str());
      if(!m_bAuthWarningDone)
      {
        m_bAuthWarningDone=true;
        m_bConnectionWarningDone=true; // eat "handshake not ready" message in this case
        CStdString strMsg=g_localizeStrings.Get(15206); // Bad authorization: Check username and password
        StatusUpdate(strMsg);
      }
      break;
    case S_SUBMIT_BAD_PASSWORD:
      CLog::Log(LOGERROR, "AudioScrobbler: %s", strText.c_str());
      if(!m_bBadPassWarningDone)
      {
        m_bBadPassWarningDone=true;
        CStdString strMsg=g_localizeStrings.Get(15206); // Bad authorization: Check username and password
        StatusUpdate(strMsg);
      }
      break;
    case S_HANDHAKE_NOTREADY:
      CLog::Log(LOGNOTICE, "AudioScrobbler: %s", strText.c_str());
      if(!m_bConnectionWarningDone)
      {
        m_bConnectionWarningDone=true;
        CStdString strMsg=g_localizeStrings.Get(15204); // Unable to handshake: sleeping...
        StatusUpdate(strMsg);
      }
      break;
    case S_HANDSHAKE_OLD_CLIENT:
      CLog::Log(LOGNOTICE, "AudioScrobbler: %s", strText.c_str());
      if(!m_bUpdateWarningDone)
      {
        m_bUpdateWarningDone=true;
        CStdString strMsg=g_localizeStrings.Get(15205); // Please update xbmc
        StatusUpdate(strMsg);
      }
      break;
    default:
      CLog::Log(LOGDEBUG, "AudioScrobbler: %s", strText.c_str());
      break;
  }
}

void CScrobbler::StatusUpdate(const CStdString& strText)
{
  CStdString strAudioScrobbler=g_localizeStrings.Get(15200);  // AudioScrobbler
  g_application.m_guiDialogKaiToast.QueueNotification("", strAudioScrobbler, strText, 10000);
}

void CScrobbler::WorkerThread()
{
  while (1) 
  {
    WaitForSingleObject(m_hWorkerEvent, INFINITE);
    if (m_bCloseThread)
      break;

    CHTTP http;
    CStdString strHtml;
    bool bSuccess;
    if (!m_bReadyToSubmit)
    {
      bSuccess=http.Get(m_strHsString, strHtml);
      if (bSuccess)
      {
        LPSTR lphtml=strHtml.GetBuffer();
        HandleHandshake(lphtml);
        strHtml.ReleaseBuffer();
        if (m_bReHandShaking)
          m_bReHandShaking = false;
      }
    }
    else
    {
      bSuccess=http.Post(m_strSubmitUrl, m_strSubmit, strHtml);
      if (bSuccess)
      {
        LPSTR lphtml=strHtml.GetBuffer();
        HandleSubmit(lphtml);
        strHtml.ReleaseBuffer();
        if (m_bReHandShaking) continue;
      }
    }

    if(!bSuccess)
    {
      if(m_bReadyToSubmit)
        m_bSubmitInProgress = false; // failed to post, means post is over

      StatusUpdate(S_CONNECT_ERROR,"Could not connect to server.");
    }

    // OK, if this was a handshake, it failed since m_bReadyToSubmit isn't true. Submissions get cached.
    while (!m_bReadyToSubmit)
    {
      StatusUpdate(S_HANDHAKE_NOTREADY,"Unable to handshake: sleeping...");
      // sleep for HS_FAIL_WAIT, or until we have cancelled our thread
      WaitForSingleObject(m_hWorkerEvent, HS_FAIL_WAIT);
      if (m_bCloseThread)
        return;
      // and try again.
      bSuccess=http.Get(m_strHsString, strHtml);
      if (bSuccess)
      {
        LPSTR lphtml=strHtml.GetBuffer();
        HandleHandshake(lphtml);
        strHtml.ReleaseBuffer();
      }
    }
  }
}

void CScrobbler::SetSongStartTime()
{
  time(&m_SongStartTime);
}

CStdString CScrobbler::GetConnectionState()
{
  if (!g_guiSettings.GetBool("lastfm.enable") && !g_guiSettings.GetBool("lastfm.recordtoprofile"))
    return "";

  return (m_bReadyToSubmit ? g_localizeStrings.Get(15207) : g_localizeStrings.Get(15208));  // Connected : Not Connected
}

CStdString CScrobbler::GetSubmitInterval()
{
  CStdString strInterval;

  if (!g_guiSettings.GetBool("lastfm.enable") && !g_guiSettings.GetBool("lastfm.recordtoprofile"))
    return strInterval;

  CStdString strFormat=g_localizeStrings.Get(15209);  // Submit Interval %i
  strInterval.Format(strFormat.c_str(), m_Interval);

  return strInterval;
}

CStdString CScrobbler::GetFilesCached()
{
  CStdString strCachedFiles;

  if (!g_guiSettings.GetBool("lastfm.enable") && !g_guiSettings.GetBool("lastfm.recordtoprofile"))
    return strCachedFiles;

  CStdString strFormat=g_localizeStrings.Get(15210);  // Cached %i Songs
  strCachedFiles.Format(strFormat.c_str(), m_vecSubmissionJournal.size());

  return strCachedFiles;
}

void CScrobbler::SetSecsTillSubmit(int iSecs)
{
  m_iSecsTillSubmit=iSecs;
}

CStdString CScrobbler::GetSubmitState()
{
  CStdString strText;

  if (!g_guiSettings.GetBool("lastfm.enable") && !g_guiSettings.GetBool("lastfm.recordtoprofile"))
    return strText;

  if (m_bSubmitInProgress)
  {
    strText=g_localizeStrings.Get(15211);  // Submitting...
    return strText;
  }

  if (m_bReadyToSubmit && m_bShouldSubmit)
  {
    CStdString strFormat=g_localizeStrings.Get(15212);  // Submitting in %i secs
    strText.Format(strFormat.c_str(), m_iSecsTillSubmit);
    return strText;
  }

  return strText;
}

void CScrobbler::SetSubmitSong(bool bSubmit)
{
  m_bShouldSubmit=bSubmit;
  m_iSecsTillSubmit=0;
}

bool CScrobbler::ShouldSubmit()
{
  return m_bShouldSubmit;
}

CStdString CScrobbler::GetTempFileName()
{
  CStdString strFileName = g_settings.GetProfileUserDataFolder();
  return CUtil::AddFileToFolder(strFileName, "Scrobbler.tmp");
}

CStdString CScrobbler::GetJournalFileName()
{
  CStdString strFileName = g_settings.GetProfileUserDataFolder();
  return CUtil::AddFileToFolder(strFileName, "Scrobbler.xml");
}

