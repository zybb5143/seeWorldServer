#include <drogon/drogon.h>
#include <drogon/WebSocketController.h>
#include <iostream>
#include <fstream>
#include <thread>
#include <map>
#include <nlohmann/json.hpp>
using json = nlohmann::json;
using namespace drogon;
#define server_debug
enum class SenderStates
{
    OCCUPIED,
    CHARGING,
    FREE,
    DISCONNECTED
};
enum class ReceiverStates
{
    CONNECTED,
    FREE
};
namespace WebSocketConnetion
{
    class SenderInfo
    {
    public:
        std::weak_ptr<drogon::WebSocketConnection> receiverPtr;
        std::weak_ptr<drogon::WebSocketConnection> senderPtr;
        int id;
        int user_id; // preserved for future work, could be used for concurrency control and validation;
        int battery;
        SenderStates state;
        SenderInfo()
        {
            state = SenderStates::DISCONNECTED;
        }
    };
    class ReceiverInfo
    {
    public:
        /* senderPtr seems redundant*/
        // std::weak_ptr<drogon::WebSocketConnection> senderPtr;
        int mobile_id;
        int user_id;
        ReceiverStates state;
        ReceiverInfo()
        {
            this->state = ReceiverStates::FREE;
            this->mobile_id = -1;
            user_id = 0;
        }
    };

    class connect : public drogon::WebSocketController<connect>
    {
        static const uint32_t MAX_NUMBER = 10u;
        std::mutex mutex[MAX_NUMBER];
        std::mutex sender_mutex;
        std::mutex receiver_mutex;
        std::map<std::weak_ptr<WebSocketConnection>, int, std::owner_less<std::weak_ptr<WebSocketConnection>>> senderPtr2SenderID;
        SenderInfo senders[MAX_NUMBER];
        std::map<std::weak_ptr<WebSocketConnection>, ReceiverInfo, std::owner_less<std::weak_ptr<WebSocketConnection>>> receiverPtr2ReceiverInfo;
        json gatherMobileState()
        {
            json jmsg = {{"type", "mobile_states"}};
            json mobile_info;
            {
                for (uint32_t i = 0; i < MAX_NUMBER; i++)
                {
                    std::lock_guard<std::mutex> lg(mutex[i]);
                    mobile_info.push_back({{"mobile_id", i}, {"state", senders[i].state}});
                }
            }
            jmsg.push_back({"mobile_info", mobile_info});
            return jmsg;
        }
        void broadcastMobileStateChange()
        {
            json states = gatherMobileState();
            std::string states_str = states.dump();
            {
                std::lock_guard<std::mutex> lg(receiver_mutex);
                for_each(receiverPtr2ReceiverInfo.begin(), receiverPtr2ReceiverInfo.end(), [&](auto &_pair)
                         { _pair.first.lock()->send(states_str); });
            }
        }

    public:
        void handleNewMessage(const WebSocketConnectionPtr &wsConnPtr, std::string &&message, const WebSocketMessageType &type)
        {
            if (message == "" || message == "\x00\x00\x00\x00" || message == "\x00\x00" || message.find('{') == -1) // heatbeats
            {
                return;
            }
#ifdef server_debug
            printf("[on-message]%s\n", message.c_str());
#endif
            json jmsg = json::parse(message);
            bool connection_success = false;
            std::string msg_type = jmsg["type"].get<std::string>();
            if (msg_type == "establish_connection")
            {
                // int mobile_id = atoi(jmsg["mobile_id"].get<std::string>().c_str());
                int mobile_id = jmsg["mobile_id"].get<int>();
                {
                    // mobile mutex
                    std::lock_guard<std::mutex> lg(mutex[mobile_id]);
                    if (senders[mobile_id].state == SenderStates::FREE)
                    {
                        connection_success = 1;
                        senders[mobile_id].receiverPtr = wsConnPtr;
                        senders[mobile_id].state = SenderStates::OCCUPIED;
                        if (senders[mobile_id].senderPtr.expired() == false)
                        {
                            senders[mobile_id].senderPtr.lock()->send(message);
                        }
                        {
                            // receiver_mutex
                            std::lock_guard<std::mutex> lg1(receiver_mutex);
                            receiverPtr2ReceiverInfo[wsConnPtr].mobile_id = mobile_id;
                            // receiverPtr2ReceiverInfo[wsConnPtr].senderPtr = senders[mobile_id].senderPtr;
                        }
                    }
                    else
                    {
                        connection_success = false;
                    }
                }
                if (connection_success)
                {
                    broadcastMobileStateChange();
                }
                json jresp = {{"type", "connection_response"}, {"result", (connection_success ? "success" : "false")}};
                wsConnPtr->send(jresp.dump());
            }
            else if (msg_type == "close_connection")
            {
                std::weak_ptr<WebSocketConnection> peer_ptr;
                bool is_sender = false;
                int mobile_id = -1;
                {
                    std::lock_guard<std::mutex> lg(sender_mutex);
                    if (senderPtr2SenderID.find(wsConnPtr) != senderPtr2SenderID.end())
                    {
                        // for what reason to close the connection? reasons should be specified anyway.
                        mobile_id = senderPtr2SenderID[wsConnPtr];
                        is_sender = 1;
                    }
                }
                if (is_sender)
                {
                    std::lock_guard<std::mutex> lg(mutex[mobile_id]);
                    peer_ptr = senders[mobile_id].receiverPtr;
                    senders[mobile_id].state = SenderStates::FREE;
                    {
                        std::lock_guard<std::mutex> lg1(receiver_mutex);
                        receiverPtr2ReceiverInfo[peer_ptr].state = ReceiverStates::FREE;
                    }
                }
                else // receiver close the connection
                {
                    {
                        std::lock_guard<std::mutex> lg(receiver_mutex);
                        mobile_id = receiverPtr2ReceiverInfo[wsConnPtr].mobile_id;
                        receiverPtr2ReceiverInfo[wsConnPtr].mobile_id = -1;
                    }
                    {
                        std::lock_guard<std::mutex> lg(mutex[mobile_id]);
                        senders[mobile_id].state = SenderStates::FREE;
                        senders[mobile_id].receiverPtr.reset();
                        senders[mobile_id].senderPtr.lock()->send(message);
                    }
                }
                broadcastMobileStateChange();
            }
            else if (msg_type == "offer" || msg_type == "answer" || msg_type == "control")
            {
                bool is_sender = false;
                int mobile_id = -1;
                std::weak_ptr<WebSocketConnection> peer_ptr;
                {
                    std::lock_guard<std::mutex> lg(sender_mutex);
                    if (senderPtr2SenderID.find(wsConnPtr) != senderPtr2SenderID.end())
                    {
                        is_sender = true;
                        mobile_id = senderPtr2SenderID[wsConnPtr];
                    }
                }
                if (is_sender)
                {
                    {
#ifdef server_debug
                        printf("from sender to receiver\n");
#endif
                        std::lock_guard<std::mutex> lg(mutex[mobile_id]);
                        if (senders[mobile_id].receiverPtr.expired() == false)
                        {
                            senders[mobile_id].receiverPtr.lock()->send(message);
                        }
                    }
                }
                else
                {
                    {
                        std::lock_guard<std::mutex> lg(receiver_mutex);
                        mobile_id = receiverPtr2ReceiverInfo[wsConnPtr].mobile_id;
                    }
                    {
                        std::lock_guard<std::mutex> lg(mutex[mobile_id]);
                        if (senders[mobile_id].senderPtr.expired() == false)
                        {
                            senders[mobile_id].senderPtr.lock()->send(message);
                        }
                    }
                }
            }
        }
        void handleNewConnection(const HttpRequestPtr &req, const WebSocketConnectionPtr &wsConnPtr)
        {
            if (req->getPath() == "/receiver/connect")
            {
                {

                    std::lock_guard<std::mutex> lg(receiver_mutex);
                    receiverPtr2ReceiverInfo[wsConnPtr];
                }
                json jmsg = gatherMobileState();
                wsConnPtr->send(jmsg.dump());
            }
            if (req->getPath() == "/sender/connect")
            {
                int cur_mobile_id = atoi(req->getParameter("mobile_id").c_str());
                {
// mobile mutex
#ifdef server_debug
                    printf("new mobile connected, [mobile-id: %d]\n", cur_mobile_id);
#endif
                    std::lock_guard<std::mutex> lg(mutex[cur_mobile_id]);
                    senders[cur_mobile_id].senderPtr = wsConnPtr;
                    senders[cur_mobile_id].state = SenderStates::FREE;
                    {
                        std::lock_guard<std::mutex> lg1(sender_mutex);
                        senderPtr2SenderID[wsConnPtr] = cur_mobile_id;
                    }
                }
                broadcastMobileStateChange();
            }
        }
        void handleConnectionClosed(const WebSocketConnectionPtr &wsConnPtr)
        {
#ifdef server_debug
            printf("handle closed connection\n");
#endif
            bool is_sender = false;
            json jmsg = {{"type", "close_connection"}};
            int mobile_id = -1;
            {
                std::lock_guard<std::mutex> lg(sender_mutex);
                if (senderPtr2SenderID.find(wsConnPtr) != senderPtr2SenderID.end())
                {
                    is_sender = true;
                    mobile_id = senderPtr2SenderID[wsConnPtr];
                    senderPtr2SenderID.erase(wsConnPtr);
                }
            }
            if (is_sender)
            {
                printf("sender close connection server, [mobile-id=%d]\n", mobile_id);
                {
                    std::lock_guard<std::mutex> lg(mutex[mobile_id]);
                    if (senders[mobile_id].receiverPtr.expired() == false)
                    {
                        senders[mobile_id].receiverPtr.lock()->send(jmsg.dump());
                    }
                    senders[mobile_id].receiverPtr.lock().reset();
                    senders[mobile_id].senderPtr.lock().reset();
                    senders[mobile_id].user_id = -1;
                    senders[mobile_id].state = SenderStates::DISCONNECTED;
                }
                broadcastMobileStateChange();
            }
            else
            {
                bool state_changed = false;
                {

                    std::lock_guard<std::mutex> lg(receiver_mutex);
                    mobile_id = receiverPtr2ReceiverInfo[wsConnPtr].mobile_id;
#ifdef server_debug
                    printf("receiver-disconnected, mobile_id = %d\n", mobile_id);
#endif
                    receiverPtr2ReceiverInfo.erase(wsConnPtr);
                    if (mobile_id == -1)
                    {
                        return;
                    }
                }
                {
                    std::lock_guard<std::mutex> lg(mutex[mobile_id]);
                    if (senders[mobile_id].senderPtr.expired() == false)
                    {
                        senders[mobile_id].senderPtr.lock()->send(jmsg.dump());
                    }
                    senders[mobile_id].receiverPtr.lock().reset();
                    senders[mobile_id].senderPtr.lock().reset();
                    senders[mobile_id].user_id = -1;
                    senders[mobile_id].state = SenderStates::FREE;
                    state_changed = 1;
                }
                if (state_changed)
                {
                    broadcastMobileStateChange();
                }
            }
        }
        WS_PATH_LIST_BEGIN
        WS_PATH_ADD("/receiver/connect", Get);
        WS_PATH_ADD("/sender/connect", Get);
        WS_PATH_LIST_END
    };
}

int main()
{
    std::string receiverHTMLPath = "../receiver.html";
    std::string receiverFileData;
    auto readReceiverHTML = [&]()
    {
        receiverFileData = "";
        std::ifstream fin;
        fin.open(receiverHTMLPath, std::ios::in);
        std::string cur_line = "";
        while (std::getline(fin, cur_line))
        {
            receiverFileData.append(cur_line).append("\n");
        }
    };

    drogon::app().addListener("0.0.0.0", 1245);
    drogon::app().registerHandler(
        "/receiver",
        [&](const drogon::HttpRequestPtr &req,
            std::function<void(const drogon::HttpResponsePtr &)> &&callback)
        {
            readReceiverHTML();
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k200OK);
            resp->setContentTypeCode(drogon::CT_TEXT_HTML);
            resp->setBody(receiverFileData);
            callback(resp);
        },
        {drogon::Get});
    drogon::app().run();
    return 0;
}
