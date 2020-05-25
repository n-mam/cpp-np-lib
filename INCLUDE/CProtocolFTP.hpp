#ifndef FTPSEGMENTER_HPP
#define FTPSEGMENTER_HPP

#include <CProtocol.hpp>
#include <CListener.hpp>

#include <list>
#include <tuple>
#include <atomic>
#include <vector>
#include <string>
#include <functional>

namespace NPL {

enum class ESSL : uint8_t
{
  None = 0,
  Implicit,
  Explicit
};

enum class EDCProt : uint8_t
{
  Clear = 0,
  Protected
};

using TTransferCbk = std::function<bool (const char *, size_t)>;
using TResponseCbk = std::function<void (const std::string&)>;

class CProtocolFTP : public CProtocol<uint8_t, uint8_t>
{
  public:

    CProtocolFTP()
    {
      iJobInProgress.test_and_set();
    }

    virtual ~CProtocolFTP() {}

    virtual void Upload(TTransferCbk cbk, const std::string& fRemote, const std::string& fLocal, EDCProt P = EDCProt::Clear)
    {
      std::lock_guard<std::mutex> lg(iLock);

      if (!fRemote.size() || !fLocal.size()) assert(false);

      SetDCProtLevel(P);

      iJobQ.emplace_back("PASV", "", "", nullptr, nullptr);

      iJobQ.emplace_back("STOR", fRemote, fLocal, nullptr, cbk);

      ProcessNextJob();
    }

    virtual void Download(TTransferCbk cbk, const std::string& fRemote, const std::string& fLocal, EDCProt P = EDCProt::Clear)
    {
      std::lock_guard<std::mutex> lg(iLock);

      if (!fRemote.size() || (!cbk && !fLocal.size())) assert(false);

      SetDCProtLevel(P);

      iJobQ.emplace_back("PASV", "", "", nullptr, nullptr);

      iJobQ.emplace_back("RETR", fRemote, fLocal, nullptr, cbk);

      ProcessNextJob();
    }

    virtual void List(TTransferCbk cbk, const std::string& fRemote = "", EDCProt P = EDCProt::Clear)
    {
      std::lock_guard<std::mutex> lg(iLock);

      if (!cbk) assert(false);

      SetDCProtLevel(P);

      iJobQ.emplace_back("PASV", "", "", nullptr, nullptr);

      iJobQ.emplace_back("LIST", fRemote, "", nullptr, cbk);

      ProcessNextJob();        
    }

    virtual void GetCurrentDir(TResponseCbk cbk = nullptr)
    {
      std::lock_guard<std::mutex> lg(iLock);

      iJobQ.emplace_back("PWD", "", "", cbk, nullptr);

      ProcessNextJob();
    }

    virtual void SetCurrentDir(const std::string& dir, TResponseCbk cbk = nullptr)
    {
      std::lock_guard<std::mutex> lg(iLock);

      iJobQ.emplace_back("CWD", dir, "", cbk, nullptr);

      ProcessNextJob();       
    }

    virtual void CreateDir(const std::string& dir, TResponseCbk cbk)
    {
      std::lock_guard<std::mutex> lg(iLock);

      iJobQ.emplace_back("MKD", dir, "", cbk, nullptr);

      ProcessNextJob();    
    }

    virtual void RemoveDir(const std::string& dir, TResponseCbk cbk)
    {
      std::lock_guard<std::mutex> lg(iLock);

      iJobQ.emplace_back("RMD", dir, "", cbk, nullptr);

      ProcessNextJob();  
    }

    virtual void Quit(TResponseCbk cbk = nullptr)
    {
      std::lock_guard<std::mutex> lg(iLock);

      iJobQ.emplace_back("QUIT", "", "", cbk, nullptr);

      ProcessNextJob();       
    }

    virtual void SetFTPSType(ESSL ftps)
    {
      iSSLType = ftps;
    }

  protected:

    ESSL iSSLType = ESSL::None;

    EDCProt iDCProtection = EDCProt::Clear;

    bool iContinueTransfer = false;

    SPCDevice iFileDevice = nullptr;

    SPCDeviceSocket iDataChannel = nullptr;

    std::atomic_flag iJobInProgress = ATOMIC_FLAG_INIT;

    std::atomic_char iPendingResponse = 0;

    uint64_t iCurrentFileOffset = 0;

    std::list<
      std::tuple<
       std::string,      // command
       std::string,      // fRemote
       std::string,      // fLocal
       TResponseCbk,     // rcbk
       TTransferCbk      // ucbk
    >> iJobQ;

    using TStateFn = std::function<void (void)>;

    struct Transition
    {
      std::string  iTransitionState;
      char         iResponseCode;
      std::string  iNextState;
      TStateFn     iTransitionFn;
    };

    Transition FSM[27] =
    {
      // Connection states
      { "CONNECTED" , '1', "CONNECTED" , nullptr                                        },
      { "CONNECTED" , '2', "CHECK"     , [this](){ CheckExplicitFTPS(); }               },
      { "CONNECTED" , '4', "CONNECTED" , nullptr                                        },
      { "AUTH"      , '2', "TLS"       , [this](){ DoCCHandshake();     }               },
      // USER states
      { "USER"      , '1', "USER",       [this](){ }                                    },
      { "USER"      , '2', "READY",      [this](){ }                                    },
      { "USER"      , '3', "PASS",       [this](){ SendCommand("PASS", iPassword); }    },
      { "USER"      , '4', "USER",       [this](){ }                                    },
      { "USER"      , '5', "USER",       [this](){ }                                    },
      // PASS states
      { "PASS"      , '1', "USER",       [this]() { ProcessLoginEvent(false); }          },
      { "PASS"      , '2', "READY" ,     [this]() { ProcessLoginEvent(true); }          },
      { "PASS"      , '3', "ACCT" ,      [this]() { SendCommand("ACCT");}               },
      { "PASS"      , '4', "USER",       [this]() { ProcessLoginEvent(false); }          },
      { "PASS"      , '5', "USER",       [this]() { ProcessLoginEvent(false); }          },
      // PASV states
      { "PASV"      , '1', "DATA"  ,     [this]() { SkipCommand(2);        }            },      
      { "PASV"      , '2', "DATA"  ,     [this]() { ProcessPasvResponse(); }            },
      { "PASV"      , '4', "READY"  ,    [this]() { SkipCommand(2);        }            },
      { "PASV"      , '5', "READY"  ,    [this]() { SkipCommand(2);        }            },
      // DATA command (LIST RETR STOR) states
      { "DATA"      , '1', "DATA"  ,     [this] () { ProcessDataCmdResponse('1'); }        },
      { "DATA"      , '2', "DATA" ,      [this] () { ProcessDataCmdResponse('2'); }        },
      { "DATA"      , '4', "READY" ,     [this] () { ProcessDataCmdResponse('4'); }        },
      { "DATA"      , '5', "READY" ,     [this] () { ProcessDataCmdResponse('5'); }        },
      { "GEN"       , '1', "READY" ,     [this] () { ProcessGenCmdEvent();  }           },
      { "GEN"       , '2', "READY" ,     [this] () { ProcessGenCmdEvent();  }           },
      { "GEN"       , '3', "READY" ,     [this] () { ProcessGenCmdEvent();  }           },
      { "GEN"       , '4', "READY" ,     [this] () { ProcessGenCmdEvent();  }           },
      { "GEN"       , '5', "READY" ,     [this] () { ProcessGenCmdEvent();  }           }
    };

    virtual void StateMachine(const std::vector<uint8_t>& msg) override
    {
      for (size_t i = 0; i < msg.size(); i++)
      {
        std::cout << msg[i];
      }

      std::cout << "\n";

      for (int i = 0; i < sizeof(FSM) / sizeof(FSM[0]); i++)
      {
        Transition t = FSM[i];

        if ((iProtocolState == t.iTransitionState) && (t.iResponseCode == msg[0]))
        {
          iProtocolState = t.iNextState;

          if (t.iTransitionFn)
          {
            t.iTransitionFn();
          }

          break;
        }
      }      
    }

    virtual bool IsMessageComplete(const std::vector<uint8_t>& b) override
    {
      size_t l = b.size();

      if (l >= 4)
      {
        if ((b[l-2] == '\r') && (b[l-1] == '\n'))
        {
          uint8_t code[4] = { 0, 0, 0, ' '};

          memmove(code, b.data(), 3);

          for (size_t i = 0; i < l; i++)
          {
            if (0 == memcmp(b.data() + i, code, 4))
            {
              return true;
            }
          }
        }
      }
      return false;
    }

    virtual void SendCommand(const std::string& c, const std::string& arg = "")
    {
      auto cmd = c + " " + arg + "\r\n";
      std::cout << cmd;
      Write((uint8_t *)cmd.c_str(), cmd.size(), 0);
    }

    virtual void SetDCProtLevel(EDCProt P)
    {
      if (iSSLType == ESSL::Implicit || 
          iSSLType == ESSL::Explicit)
      {
        iJobQ.emplace_back("PBSZ", "0", "", nullptr, nullptr);

        auto level = (P == EDCProt::Clear) ? "C" : "P";

        iJobQ.emplace_back("PROT", level, "", 
          [this, lvl = level](const std::string& res){
            if (res[0] == '2')
            {
              iDCProtection = (lvl == "C") ? EDCProt::Clear : EDCProt::Protected;
            }
          }, nullptr);
      }
    }

    virtual void SkipCommand(int count = 1)
    {
      for (int i = 0; i < count; i++)
      {
        iJobQ.pop_front();
        iPendingResponse--;
      }

      assert(iPendingResponse.load() == 0);

      iJobInProgress.clear();

      ProcessNextJob();
    }

    virtual void ProcessNextJob(void)
    {
      if (iJobInProgress.test_and_set() == false)
      {
        assert(iPendingResponse.load() == 0);

        if (!iJobQ.size())
        {
          iJobInProgress.clear();
          return;
        }

        auto& [cmd, fRemote, fLocal, rcbk, tcbk] = iJobQ.front();

        iPendingResponse = 1;

        if (IsTransferCommand(cmd))
        {       
          iProtocolState = "DATA";
        }
        else if (cmd == "PASV")
        {
          iProtocolState = "PASV";
        }
        else
        {
          iProtocolState = "GEN";
        }

        SendCommand(cmd, fRemote);
      }
    }

    virtual void CheckExplicitFTPS(void)
    {
      if (iSSLType == ESSL::Explicit)
      {
        iProtocolState = "AUTH";        
        SendCommand("AUTH", "TLS");
      }
      else
      {
        iProtocolState = "USER";        
        SendCommand("USER", iUserName);
      }
    }

    virtual void ProcessGenCmdEvent(void)
    {
      auto& [cmd, fRemote, fLocal, rcbk, tcbk] = iJobQ.front();

      if (rcbk)
      {
        auto& m = iMessages.back();
        std::string res(m.begin(), m.end());
        rcbk(res);
      }

      SkipCommand(1);
    }

    virtual void ProcessPasvResponse(void)
    {
      auto& m = iMessages.back();

      std::string pasv(m.begin(), m.end());

      auto spec = pasv.substr(pasv.find('('));

      uint32_t h1, h2, h3, h4, p1, p2;

      int fRet = sscanf(spec.c_str(), "(%d,%d,%d,%d,%d,%d)", &h1, &h2, &h3, &h4, &p1, &p2);

      if (fRet < 6)
      {
        std::cout << "Faled to parse PASV response\n";
      }

      auto host = std::to_string(h1) + "." + 
                  std::to_string(h2) + "." +
                  std::to_string(h3) + "." +
                  std::to_string(h4);

      int port = (p1 << 8) + p2;

      SkipCommand(1);

      OpenDataChannel(host, port);
    }

    virtual void ProcessDataCmdResponse(char code = '0')
    {
      bool abort = false;

      switch (code)
      {
        case '1':
        {
          if (iDCProtection == EDCProt::Protected)
          {
            iDataChannel->InitializeSSL(
              [this] () {
                TriggerDataTransfer();
              });
          }
          else
          {
            TriggerDataTransfer();
          }

          break;
        }
        case '2':
        case '4':
        case '5':
        {
          iPendingResponse--;

          if (!IsResponsePositive(code))
          {
            abort = true;
          }

          break;
        }
      }

      if (!iPendingResponse && (!iDataChannel->IsConnected() || abort))
      {
        ResetDataChannel();

        if (iFileDevice)
        {
          iFileDevice->MarkRemoveAllListeners();
          iFileDevice->MarkRemoveSelfAsListener();
          iFileDevice.reset();
        }

        if (abort)
        {
          iJobQ.clear();
        }
        else
        {
          iJobQ.pop_front();
        }

        iJobInProgress.clear();

        ProcessNextJob();
      }
    }

    virtual void OpenDataChannel(const std::string& host, int port)
    {
      iDataChannel = std::make_shared<CDeviceSocket>();

      iDataChannel->SetName("dc");

      auto observer = std::make_shared<CListener>(
        [this]() {
          OnDataChannelConnect();
        },
        [this](const uint8_t *b, size_t n) {
          OnDataChannelRead(b, n);
        },
        [this](const uint8_t *b, size_t n) {
          OnDataChannelWrite(b, n);
        },
        [this](){
          OnDataChannelDisconnect();
        });

      observer->SetName("dc-ob");

      auto D = GetDispatcher();

      D->AddEventListener(iDataChannel)->AddEventListener(observer);

      iDataChannel->SetHostAndPort(host, port);

      iDataChannel->StartSocketClient();
    }

    virtual void OnDataChannelConnect(void)
    {
      auto& [cmd, fRemote, fLocal, rcbk, tcbk] = iJobQ.front();

      if (fLocal.size())
      {
        assert(!iFileDevice);

        if (cmd == "LIST") assert(!fLocal.size());

        iFileDevice = std::make_shared<CDevice>(
          fLocal.c_str(),
          cmd == "RETR" ? true : false);

        iFileDevice->SetName("fl");

        iCurrentFileOffset = 0;

        auto observer = std::make_shared<CListener>(
          nullptr,
          [this] (const uint8_t *b, size_t n) {
            OnFileRead(b, n);
          },
          [this] (const uint8_t *b, size_t n) {
            OnFileWrite(b, n);
          },
          [this] () {
            OnFileDisconnect();
          }
        );

        observer->SetName("fl-ob");

        auto D = GetDispatcher();

        D->AddEventListener(iFileDevice)->AddEventListener(observer);
      }

      iContinueTransfer = true;
    }

    virtual void OnDataChannelRead(const uint8_t *b, size_t n)
    {
      auto& [cmd, fRemote, fLocal, rcbk, tcbk] = iJobQ.front();

      if (tcbk)
      {
        if (iContinueTransfer)
        {
          iContinueTransfer = tcbk((const char *)b, n);
        }
        else
        {
          iDataChannel->Shutdown();
          return;
        }
      }

      if (iFileDevice)
      {
        iFileDevice->Write(b, n, iCurrentFileOffset);
        iCurrentFileOffset += n;
      }      
    }

    virtual void OnDataChannelWrite(const uint8_t *b, size_t n)
    {
      auto& [cmd, fRemote, fLocal, rcbk, tcbk] = iJobQ.front();
    }

    virtual void OnDataChannelDisconnect(void)
    {
      auto& [cmd, fRemote, fLocal, rcbk, tcbk] = iJobQ.front();

      if (tcbk)
      {
        tcbk(nullptr, 0);
      }

      ProcessDataCmdResponse();
    }

    virtual void OnFileRead(const uint8_t *b, size_t n)
    {
      auto& [cmd, fRemote, fLocal, rcbk, tcbk] = iJobQ.front();

      if (tcbk)
      {
        if (iContinueTransfer)
        {
          iContinueTransfer = tcbk((const char *)b, n);
        }
        else
        {
          iDataChannel->Shutdown();
          return;          
        }
      }

      iDataChannel->Write(b, n);

      iCurrentFileOffset += n;

      iFileDevice->Read(nullptr, 0, iCurrentFileOffset);
    }

    virtual void OnFileWrite(const uint8_t *b, size_t n)
    {

    }

    virtual void OnFileDisconnect(void)
    {
      iDataChannel->Shutdown();
    }

    virtual void TriggerDataTransfer(void)
    {
      auto& [cmd, fRemote, fLocal, rcbk, tcbk] = iJobQ.front();

      if (cmd == "STOR")
      {
        iFileDevice->Read(nullptr, 0, iCurrentFileOffset);
      }
    }

    virtual void ProcessLoginEvent(bool status)
    {
      if (status)
      {
        NotifyState("PASS", 'S');
        iJobInProgress.clear();     
        ProcessNextJob();
      }
      else
      {
        NotifyState("PASS", 'F');
        iJobQ.clear();
        iJobInProgress.clear();     
      }      
    }

    virtual void ResetDataChannel(void)
    {
      iDataChannel->MarkRemoveAllListeners();
      iDataChannel->MarkRemoveSelfAsListener();
      iDataChannel.reset();
    }

    virtual bool IsTransferCommand(const std::string& cmd)
    {
      return (cmd == "RETR" || 
              cmd == "LIST" ||
              cmd == "STOR");
    }

    virtual bool IsResponsePositive(char c)
    {
      return (c == '2');
    }

    virtual void OnConnect(void) override
    {
      CProtocol::OnConnect();

      if (iSSLType == ESSL::Implicit)
      {
        DoCCHandshake();
      }
    }

    virtual void DoCCHandshake()
    {
      auto cc = iTarget.lock();

      if (cc)
      {
        auto sock = std::dynamic_pointer_cast<CDeviceSocket>(cc);

        assert(sock);

        sock->InitializeSSL([this] () {
          if (iSSLType == ESSL::Explicit)
          {
            iProtocolState = "USER";
            SendCommand("USER", iUserName);
          }
        });
      }
    }
};

using SPCProtocolFTP = std::shared_ptr<CProtocolFTP>;

} //npl namespace

#endif //FTPSEGMENTER_HPP