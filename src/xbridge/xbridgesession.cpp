// Copyright (c) 2017-2019 The Blocknet developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//*****************************************************************************
//*****************************************************************************

#include <xbridge/xbridgesession.h>

#include <xbridge/bitcoinrpcconnector.h>
#include <xbridge/util/fastdelegate.h>
#include <xbridge/util/posixtimeconversion.h>
#include <xbridge/util/xutil.h>
#include <xbridge/util/logger.h>
#include <xbridge/util/txlog.h>
#include <xbridge/util/xassert.h>
#include <xbridge/xbitcointransaction.h>
#include <xbridge/xbridgeapp.h>
#include <xbridge/xbridgeexchange.h>
#include <xbridge/xbridgepacket.h>
#include <xbridge/xuiconnector.h>

#include <base58.h>
#include <consensus/validation.h>
#include <random.h>
#include <rpc/protocol.h>
#include <script/script.h>
#include <servicenode/servicenodemgr.h>
#include <sync.h>

#include <json/json_spirit.h>
#include <json/json_spirit_reader_template.h>
#include <json/json_spirit_writer_template.h>
#include <json/json_spirit_utils.h>

#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/date_time/posix_time/conversion.hpp>

using namespace json_spirit;

//*****************************************************************************
//*****************************************************************************
namespace xbridge
{

//******************************************************************************
//******************************************************************************
// Threshold for nLockTime: below this value it is interpreted as block number,
// otherwise as UNIX timestamp.
// Tue Nov  5 00:53:20 1985 UTC
// const unsigned int LOCKTIME_THRESHOLD = 500000000;

//******************************************************************************
//******************************************************************************
struct PrintErrorCode
{
    const boost::system::error_code & error;

    explicit PrintErrorCode(const boost::system::error_code & e) : error(e) {}

    friend std::ostream & operator<<(std::ostream & out, const PrintErrorCode & e)
    {
        return out << " ERROR <" << e.error.value() << "> " << e.error.message();
    }
};

//*****************************************************************************
//*****************************************************************************
class Session::Impl
{
    friend class Session;

protected:
    void init();

protected:
    void sendPacket(const std::vector<unsigned char> & to, const XBridgePacketPtr & packet) const;
    void sendPacketBroadcast(XBridgePacketPtr packet) const;

    // return true if packet not for me, relayed
    bool checkPacketAddress(XBridgePacketPtr packet) const;

    // fn search xaddress in transaction and restore full 'coin' address as string
    bool isAddressInTransaction(const std::vector<unsigned char> & address,
                                const TransactionPtr & tx) const;

protected:
    bool encryptPacket(XBridgePacketPtr packet) const;
    bool decryptPacket(XBridgePacketPtr packet) const;

protected:
    bool processInvalid(XBridgePacketPtr packet) const;
    bool processZero(XBridgePacketPtr packet) const;
    bool processXChatMessage(XBridgePacketPtr packet) const;
    bool processServicesPing(XBridgePacketPtr packet) const;

    bool processTransaction(XBridgePacketPtr packet) const;
    bool processPendingTransaction(XBridgePacketPtr packet) const;
    bool processTransactionAccepting(XBridgePacketPtr packet) const;

    bool processTransactionHold(XBridgePacketPtr packet) const;
    bool processTransactionHoldApply(XBridgePacketPtr packet) const;

    bool processTransactionInit(XBridgePacketPtr packet) const;
    bool processTransactionInitialized(XBridgePacketPtr packet) const;

    bool processTransactionCreateA(XBridgePacketPtr packet) const;
    bool processTransactionCreateB(XBridgePacketPtr packet) const;
    bool processTransactionCreatedA(XBridgePacketPtr packet) const;
    bool processTransactionCreatedB(XBridgePacketPtr packet) const;

    bool processTransactionConfirmA(XBridgePacketPtr packet) const;
    bool processTransactionConfirmedA(XBridgePacketPtr packet) const;

    bool processTransactionConfirmB(XBridgePacketPtr packet) const;
    bool processTransactionConfirmedB(XBridgePacketPtr packet) const;

    bool finishTransaction(TransactionPtr tr) const;

    bool sendCancelTransaction(const TransactionPtr & tx,
                               const TxCancelReason & reason) const;
    bool sendCancelTransaction(const TransactionDescrPtr & tx,
                               const TxCancelReason & reason) const;

    bool processTransactionCancel(XBridgePacketPtr packet) const;

    bool processTransactionFinished(XBridgePacketPtr packet) const;

protected:
    bool redeemOrderDeposit(const TransactionDescrPtr & xtx, int32_t & errCode) const;
    bool redeemOrderCounterpartyDeposit(const TransactionDescrPtr & xtx, int32_t & errCode) const;
    bool refundTraderDeposit(const std::string & orderId, const std::string & currency, const uint32_t & lockTime,
                             const std::string & refTx, int32_t & errCode) const;
    void sendTransaction(uint256 & id) const;

protected:
    std::vector<unsigned char> m_myid;

    typedef fastdelegate::FastDelegate1<XBridgePacketPtr, bool> PacketHandler;
    typedef std::map<const int, PacketHandler> PacketHandlersMap;
    PacketHandlersMap m_handlers;
};

//*****************************************************************************
//*****************************************************************************
Session::Session()
    : m_p(new Impl)
    , m_isWorking(false)
{
    m_p->init();
}

//*****************************************************************************
//*****************************************************************************
Session::~Session()
{
}

//*****************************************************************************
//*****************************************************************************
const std::vector<unsigned char> & Session::sessionAddr() const
{
    return m_p->m_myid;
}

//*****************************************************************************
//*****************************************************************************
void Session::Impl::init()
{
    if (m_handlers.size())
    {
        LOG() << "packet handlers map must be empty" << __FUNCTION__;
        return;
    }

    m_myid.resize(20);
    GetStrongRandBytes(&m_myid[0], 20);

    // process invalid
    m_handlers[xbcInvalid]                   .bind(this, &Impl::processInvalid);

    if (gArgs.GetBoolArg("-enableexchange", false) && sn::ServiceNodeMgr::instance().hasActiveSn())
    {
        // server side
        m_handlers[xbcTransaction]           .bind(this, &Impl::processTransaction);
        m_handlers[xbcTransactionAccepting]  .bind(this, &Impl::processTransactionAccepting);
        m_handlers[xbcTransactionHoldApply]  .bind(this, &Impl::processTransactionHoldApply);
        m_handlers[xbcTransactionInitialized].bind(this, &Impl::processTransactionInitialized);
        m_handlers[xbcTransactionCreatedA]   .bind(this, &Impl::processTransactionCreatedA);
        m_handlers[xbcTransactionCreatedB]   .bind(this, &Impl::processTransactionCreatedB);
        m_handlers[xbcTransactionConfirmedA] .bind(this, &Impl::processTransactionConfirmedA);
        m_handlers[xbcTransactionConfirmedB] .bind(this, &Impl::processTransactionConfirmedB);
    }
    else
    {
        // client side
        m_handlers[xbcPendingTransaction]    .bind(this, &Impl::processPendingTransaction);
        m_handlers[xbcTransactionHold]       .bind(this, &Impl::processTransactionHold);
        m_handlers[xbcTransactionInit]       .bind(this, &Impl::processTransactionInit);
        m_handlers[xbcTransactionCreateA]    .bind(this, &Impl::processTransactionCreateA);
        m_handlers[xbcTransactionCreateB]    .bind(this, &Impl::processTransactionCreateB);
        m_handlers[xbcTransactionConfirmA]   .bind(this, &Impl::processTransactionConfirmA);
        m_handlers[xbcTransactionConfirmB]   .bind(this, &Impl::processTransactionConfirmB);
    }

    {
        // common handlers
        m_handlers[xbcTransactionCancel]     .bind(this, &Impl::processTransactionCancel);
        m_handlers[xbcTransactionFinished]   .bind(this, &Impl::processTransactionFinished);
    }

    // xchat ()
    m_handlers[xbcXChatMessage].bind(this, &Impl::processXChatMessage);
}

//*****************************************************************************
//*****************************************************************************
bool Session::Impl::encryptPacket(XBridgePacketPtr /*packet*/) const
{
    // DEBUG_TRACE();
    // TODO implement this
    return true;
}

//*****************************************************************************
//*****************************************************************************
bool Session::Impl::decryptPacket(XBridgePacketPtr /*packet*/) const
{
    // DEBUG_TRACE();
    // TODO implement this
    return true;
}

//*****************************************************************************
//*****************************************************************************
void Session::Impl::sendPacket(const std::vector<unsigned char> & to,
                               const XBridgePacketPtr & packet) const
{
    xbridge::App & app = xbridge::App::instance();
    app.sendPacket(to, packet);
}

//*****************************************************************************
//*****************************************************************************
void Session::Impl::sendPacketBroadcast(XBridgePacketPtr packet) const
{
    // DEBUG_TRACE();

    xbridge::App & app = xbridge::App::instance();
    app.sendPacket(packet);
}

//*****************************************************************************
// return true if packet for me and need to process
//*****************************************************************************
bool Session::Impl::checkPacketAddress(XBridgePacketPtr packet) const
{
    if (packet->size() < 20)
    {
        return false;
    }

    // check address
    if (memcmp(packet->data(), &m_myid[0], 20) == 0)
    {
        // this session address, need to process
        return true;
    }

    // not for me
    return false;
}

//*****************************************************************************
//*****************************************************************************
bool Session::processPacket(XBridgePacketPtr packet, CValidationState * state)
{
    // DEBUG_TRACE();

    setWorking();

    if (!m_p->decryptPacket(packet))
    {
        ERR() << "packet decoding error " << __FUNCTION__;
        setNotWorking();
        return false;
    }

    XBridgeCommand c = packet->command();

    if (m_p->m_handlers.count(c) == 0)
    {
        ERR() << "unknown command code <" << c << "> " << __FUNCTION__;
        m_p->m_handlers.at(xbcInvalid)(packet);
        setNotWorking();
        return false;
    }

    TRACE() << "received packet, command code <" << c << ">";

    if (!m_p->m_handlers.at(c)(packet))
    {
        if (state)
        {
            state->DoS(0, error("Xbridge packet processing error"), REJECT_INVALID, "bad-xbridge-packet");
        }

        ERR() << "packet processing error <" << c << "> " << __FUNCTION__;
        setNotWorking();
        return false;
    }

    setNotWorking();
    return true;
}

//*****************************************************************************
//*****************************************************************************
bool Session::Impl::processInvalid(XBridgePacketPtr /*packet*/) const
{
    // DEBUG_TRACE();
    // LOG() << "xbcInvalid instead of " << packet->command();
    return true;
}

//*****************************************************************************
//*****************************************************************************
bool Session::Impl::processZero(XBridgePacketPtr /*packet*/) const
{
    return true;
}

//*****************************************************************************
//*****************************************************************************
// static
bool Session::checkXBridgePacketVersion(const std::vector<unsigned char> & message)
{
    const uint32_t version = *reinterpret_cast<const uint32_t *>(&message[0]);

    if (version != static_cast<boost::uint32_t>(XBRIDGE_PROTOCOL_VERSION))
    {
        // ERR() << "incorrect protocol version <" << version << "> " << __FUNCTION__;
        return false;
    }

    return true;
}

//*****************************************************************************
//*****************************************************************************
// static
bool Session::checkXBridgePacketVersion(XBridgePacketPtr packet)
{
    if (packet->version() != static_cast<boost::uint32_t>(XBRIDGE_PROTOCOL_VERSION))
    {
        // ERR() << "incorrect protocol version <" << packet->version() << "> " << __FUNCTION__;
        return false;
    }

    return true;
}

//*****************************************************************************
// retranslate packets from wallet to xbridge network
//*****************************************************************************
bool Session::Impl::processXChatMessage(XBridgePacketPtr /*packet*/) const
{
    LOG() << "Session::Impl::processXChatMessage not implemented";
    return true;

//    DEBUG_TRACE();

//    // size must be > 20 bytes (160bit)
//    if (packet->size() <= 20)
//    {
//        ERR() << "invalid packet size for xbcXChatMessage "
//              << "need more than 20 received " << packet->size() << " "
//              << __FUNCTION__;
//        return false;
//    }

//    // read dest address
//    std::vector<unsigned char> daddr(packet->data(), packet->data() + 20);

//    XBridgeApp & app = XBridgeApp::instance();
//    app.onSend(daddr,
//               std::vector<unsigned char>(packet->header(), packet->header()+packet->allSize()));

//    return true;
}

//*****************************************************************************
// broadcast
//*****************************************************************************
bool Session::Impl::processTransaction(XBridgePacketPtr packet) const
{
    // check and process packet if bridge is exchange
    Exchange & e = Exchange::instance();
    if (!e.isStarted())
    {
        return true;
    }

    DEBUG_TRACE();

    // size must be > 152 bytes
    if (packet->size() < 152)
    {
        ERR() << "invalid packet size for xbcTransaction "
              << "need min 152 bytes, received " << packet->size() << " "
              << __FUNCTION__;
        return false;
    }

    // read packet data
    std::vector<unsigned char> sid(packet->data(), packet->data()+XBridgePacket::hashSize);
    uint256 id(sid);
    uint32_t offset = XBridgePacket::hashSize;

    // Check if order already exists, if it does ignore processing
    TransactionPtr t = e.pendingTransaction(id);
    if (t->matches(id)) {
        // Update the transaction timestamp
        if (e.updateTimestampOrRemoveExpired(t)) {
            if (!e.makerUtxosAreStillValid(t)) { // if the maker utxos are no longer valid, cancel the order
                sendCancelTransaction(t, crBadUtxo);
                return false;
            }
            LOG() << "order already received, updating timestamp " << id.ToString()
                  << " " << __FUNCTION__;
            // relay order to network
            sendTransaction(id);
        }
        return true;
    }

    // source
    std::vector<unsigned char> saddr(packet->data()+offset, packet->data()+offset+XBridgePacket::addressSize);
    offset += XBridgePacket::addressSize;
    std::string scurrency((const char *)packet->data()+offset);
    offset += 8;
    uint64_t samount = *static_cast<boost::uint64_t *>(static_cast<void *>(packet->data()+offset));
    offset += sizeof(uint64_t);

    // destination
    std::vector<unsigned char> daddr(packet->data()+offset, packet->data()+offset+XBridgePacket::addressSize);
    offset += XBridgePacket::addressSize;
    std::string dcurrency((const char *)packet->data()+offset);
    offset += 8;
    uint64_t damount = *static_cast<uint64_t *>(static_cast<void *>(packet->data()+offset));
    offset += sizeof(uint64_t);

    uint64_t timestamp = *static_cast<uint64_t *>(static_cast<void *>(packet->data()+offset));
    offset += sizeof(uint64_t);

    std::vector<unsigned char> sblockhash(packet->data()+offset, packet->data()+offset+XBridgePacket::hashSize);
    uint256 blockHash(sblockhash);
    offset += XBridgePacket::hashSize;

    std::vector<unsigned char> mpubkey(packet->pubkey(), packet->pubkey()+XBridgePacket::pubkeySize);

    if (!packet->verify(mpubkey))
    {
        WARN() << "invalid packet signature " << __FUNCTION__;
        return true;
    }

    xbridge::App & xapp = xbridge::App::instance();
    WalletConnectorPtr sconn = xapp.connectorByCurrency(scurrency);
    WalletConnectorPtr dconn = xapp.connectorByCurrency(dcurrency);
    if (!sconn || !dconn)
    {
        WARN() << "no connector for <" << (!sconn ? scurrency : dcurrency) << "> " << __FUNCTION__;
        return true;
    }

    double commonAmount = 0;

    // utxo items
    std::vector<wallet::UtxoEntry> utxoItems;
    {
        // array size
        uint32_t utxoItemsCount = *static_cast<uint32_t *>(static_cast<void *>(packet->data()+offset));
        offset += sizeof(uint32_t);

        // items
        for (uint32_t i = 0; i < utxoItemsCount; ++i)
        {
            const static uint32_t utxoItemSize = XBridgePacket::hashSize + sizeof(uint32_t) +
                                                 XBridgePacket::addressSize + XBridgePacket::signatureSize;
            if (packet->size() < offset+utxoItemSize)
            {
                WARN() << "bad packet size while reading utxo items, packet dropped in " << __FUNCTION__;
                return true;
            }

            wallet::UtxoEntry entry;

            std::vector<unsigned char> stxid(packet->data()+offset, packet->data()+offset+XBridgePacket::hashSize);
            uint256 txid(stxid);
            offset += XBridgePacket::hashSize;

            entry.txId = txid.ToString();

            entry.vout = *static_cast<uint32_t *>(static_cast<void *>(packet->data()+offset));
            offset += sizeof(uint32_t);

            entry.rawAddress = std::vector<unsigned char>(packet->data()+offset, packet->data()+offset+20);
            offset += XBridgePacket::addressSize;

            entry.address = sconn->fromXAddr(entry.rawAddress);

            entry.signature = std::vector<unsigned char>(packet->data()+offset, packet->data()+offset+XBridgePacket::signatureSize);
            offset += XBridgePacket::signatureSize;

            if (!sconn->getTxOut(entry))
            {
                LOG() << "not found utxo entry <" << entry.txId
                      << "> no " << entry.vout << " " << __FUNCTION__;
                continue;
            }

            // check signature
            std::string signature = EncodeBase64(&entry.signature[0], entry.signature.size());
            if (!sconn->verifyMessage(entry.address, entry.toString(), signature))
            {
                LOG() << "not valid signature, bad utxo entry" << entry.txId
                      << "> no " << entry.vout << " " << __FUNCTION__;
                continue;
            }

            commonAmount += entry.amount;

            utxoItems.push_back(entry);
        }
    }

    if (utxoItems.empty())
    {
        LOG() << "order rejected, utxo items are empty <" << __FUNCTION__;
        return true;
    }

    if (commonAmount * TransactionDescr::COIN < samount)
    {
        LOG() << "order rejected, amount from utxo items <" << commonAmount
              << "> less than required <" << samount << "> " << __FUNCTION__;
        return true;
    }

    // check dust amount
    if (sconn->isDustAmount(static_cast<double>(samount) / TransactionDescr::COIN) ||
        sconn->isDustAmount(commonAmount - (static_cast<double>(samount) / TransactionDescr::COIN)) ||
        dconn->isDustAmount(static_cast<double>(damount) / TransactionDescr::COIN))
    {
        LOG() << "reject dust amount order " << id.ToString() << " " << __FUNCTION__;
        return true;
    }

    LOG() << "received order " << id.GetHex() << std::endl
          << "    from " << HexStr(saddr) << std::endl
          << "             " << scurrency << " : " << samount << std::endl
          << "    to   " << HexStr(daddr) << std::endl
          << "             " << dcurrency << " : " << damount << std::endl;

    std::string saddrStr = sconn->fromXAddr(saddr);
    std::string daddrStr = dconn->fromXAddr(daddr);

    std::vector<unsigned char> firstUtxoSig = utxoItems.at(0).signature;

    CHashWriter ss(SER_GETHASH, 0);
    ss << saddrStr
       << scurrency
       << samount
       << daddrStr
       << dcurrency
       << damount
       << timestamp
       << blockHash
       << firstUtxoSig;
    uint256 checkId = ss.GetHash();
    if(checkId != id)
    {
        WARN() << "id from packet is differs from body hash:" << std::endl
               << "packet id: " << id.GetHex() << std::endl
               << "body hash:" << checkId.GetHex() << std::endl
               << __FUNCTION__;

        return true;
    }

    // check utxo items
    if (!e.checkUtxoItems(id, utxoItems))
    {
        LOG() << "order rejected, error check utxo items "  << id.ToString()
              << " " << __FUNCTION__;
        return true;
    }

    {
        bool isCreated = false;
        if (!e.createTransaction(id,
                                 saddr, scurrency, samount,
                                 daddr, dcurrency, damount,
                                 timestamp,
                                 mpubkey, utxoItems,
                                 blockHash, isCreated))
        {
            // not created
            LOG() << "failed to create order "  << id.ToString() << " " << __FUNCTION__;
            return true;
        }
        
        TransactionPtr tr = e.pendingTransaction(id);

        if (isCreated)
        {
            {
                TransactionDescrPtr d(new TransactionDescr);
                d->id           = id;
                d->fromCurrency = scurrency;
                d->fromAmount   = samount;
                d->toCurrency   = dcurrency;
                d->toAmount     = damount;
                d->state        = TransactionDescr::trPending;
                d->blockHash    = blockHash;

                LOG() << __FUNCTION__ << d;

                // Set role 'A' utxos used in the order
                tr->a_setUtxos(utxoItems);

                LOG() << __FUNCTION__ << tr;

                xuiConnector.NotifyXBridgeTransactionReceived(d);
            }
        }

        if (!tr->matches(id)) { // couldn't find order
            LOG() << "failed to find order after it was created "  << id.ToString() << " " << __FUNCTION__;
            return true;
        }

        sendTransaction(id);
    }

    return true;
}

//******************************************************************************
// broadcast
//******************************************************************************
bool Session::Impl::processPendingTransaction(XBridgePacketPtr packet) const
{
    Exchange & e = Exchange::instance();
    if (e.isEnabled())
    {
        return true;
    }

    DEBUG_TRACE();

    if (packet->size() != 124)
    {
        ERR() << "incorrect packet size for xbcPendingTransaction "
              << "need 124 received " << packet->size() << " "
              << __FUNCTION__;
        return false;
    }

    std::vector<unsigned char> stxid(packet->data(), packet->data()+XBridgePacket::hashSize);
    uint256 txid(stxid);
    uint32_t offset = XBridgePacket::hashSize;

    std::string scurrency = std::string(reinterpret_cast<const char *>(packet->data()+offset));
    offset += 8;
    uint64_t samount = *reinterpret_cast<boost::uint64_t *>(packet->data()+offset);
    offset += sizeof(uint64_t);

    std::string dcurrency = std::string(reinterpret_cast<const char *>(packet->data()+offset));
    offset += 8;
    uint64_t damount = *reinterpret_cast<boost::uint64_t *>(packet->data()+offset);
    offset += sizeof(uint64_t);

    auto hubAddress = std::vector<unsigned char>(packet->data()+offset, packet->data()+offset+XBridgePacket::addressSize);

    xbridge::App & xapp = App::instance();
    TransactionDescrPtr ptr = xapp.transaction(txid);

    // Servicenode pubkey assigned to order
    std::vector<unsigned char> spubkey(packet->pubkey(), packet->pubkey()+XBridgePacket::pubkeySize);

    // Reject if snode key doesn't match original (prevent order manipulation)
    if (ptr && !packet->verify(ptr->sPubKey)) {
        WARN() << "wrong servicenode handling order, expected " << HexStr(ptr->sPubKey)
               << " but received pubkey " << HexStr(spubkey)
               << " and hub address " << HexStr(hubAddress) << " " << __FUNCTION__;
        return true;
    }

    // All traders verify sig, important when tx ptr is not known yet
    if (!packet->verify(spubkey))
    {
        WARN() << "invalid packet signature " << __FUNCTION__;
        return true;
    }

    WalletConnectorPtr sconn = xapp.connectorByCurrency(scurrency);
    WalletConnectorPtr dconn = xapp.connectorByCurrency(dcurrency);
    if (!sconn || !dconn)
    {
        WARN() << "no connector for <" << (!sconn ? scurrency : dcurrency) << "> " << __FUNCTION__;
        return true;
    }

    if (ptr)
    {
        if (ptr->state > TransactionDescr::trPending)
        {
            LOG() << "already received order " << ptr->id.ToString() << " " << __FUNCTION__;

            LOG() << __FUNCTION__ << ptr;

            return true;
        }

        if (ptr->state == TransactionDescr::trNew)
        {
            LOG() << "received confirmed order from snode, setting status to pending " << __FUNCTION__;
            ptr->state = TransactionDescr::trPending;
        }
        
        if (ptr->state == TransactionDescr::trCancelled)
        {
            LOG() << "already received order and was cancelled " << ptr->id.ToString() << " " << __FUNCTION__;
            LOG() << __FUNCTION__ << ptr;
            return true;
        }

        // update timestamp
        ptr->updateTimestamp();

        LOG() << __FUNCTION__ << ptr;

        xuiConnector.NotifyXBridgeTransactionChanged(ptr->id);

        return true;
    }

    // create tx item
    ptr.reset(new TransactionDescr);
    ptr->id           = txid;

    ptr->fromCurrency = scurrency;
    ptr->fromAmount   = samount;

    ptr->toCurrency   = dcurrency;
    ptr->toAmount     = damount;

    ptr->hubAddress   = hubAddress;
    offset += XBridgePacket::addressSize;

    ptr->created      = xbridge::intToTime(*reinterpret_cast<boost::uint64_t *>(packet->data()+offset));
    offset += sizeof(uint64_t);

    ptr->state        = TransactionDescr::trPending;
    ptr->sPubKey      = spubkey;

    std::vector<unsigned char> sblockhash(packet->data()+offset, packet->data()+offset+XBridgePacket::hashSize);
    ptr->blockHash    = uint256(sblockhash);
    offset += XBridgePacket::hashSize;

    xapp.appendTransaction(ptr);

    LOG() << "received order <" << ptr->id.GetHex() << "> " << __FUNCTION__;

    LOG() << __FUNCTION__ << ptr;

    xuiConnector.NotifyXBridgeTransactionReceived(ptr);

    return true;
}

//*****************************************************************************
//*****************************************************************************
bool Session::Impl::processTransactionAccepting(XBridgePacketPtr packet) const
{

    // check and process packet if bridge is exchange
    Exchange & e = Exchange::instance();
    if (!e.isStarted())
    {
        return true;
    }

    DEBUG_TRACE();

    if (!checkPacketAddress(packet))
    {
        return true;
    }

    // size must be >= 164 bytes
    if (packet->size() < 164)
    {
        ERR() << "invalid packet size for xbcTransactionAccepting "
              << "need min 164 bytes, received " << packet->size() << " "
              << __FUNCTION__;
        return false;
    }

    uint32_t offset = XBridgePacket::addressSize;

    // read packet data
    std::vector<unsigned char> sid(packet->data()+offset, packet->data()+offset+XBridgePacket::hashSize);
    uint256 id(sid);
    offset += XBridgePacket::hashSize;

    // source
    std::vector<unsigned char> saddr(packet->data()+offset, packet->data()+offset+XBridgePacket::addressSize);
    offset += XBridgePacket::addressSize;
    std::string scurrency((const char *)packet->data()+offset);
    offset += 8;
    uint64_t samount = *static_cast<uint64_t *>(static_cast<void *>(packet->data()+offset));
    offset += sizeof(uint64_t);

    // destination
    std::vector<unsigned char> daddr(packet->data()+offset, packet->data()+offset+XBridgePacket::addressSize);
    offset += XBridgePacket::addressSize;
    std::string dcurrency((const char *)packet->data()+offset);
    offset += 8;
    uint64_t damount = *static_cast<uint64_t *>(static_cast<void *>(packet->data()+offset));
    offset += sizeof(uint64_t);

    std::vector<unsigned char> mpubkey(packet->pubkey(), packet->pubkey()+XBridgePacket::pubkeySize);

    // If order already accepted, ignore further attempts
    TransactionPtr trExists = e.transaction(id);
    if (trExists->matches(id)) {
        WARN() << "order already accepted " << id.GetHex() << __FUNCTION__;
        return true;
    }

    if (!packet->verify(mpubkey))
    {
        WARN() << "invalid packet signature " << __FUNCTION__;
        return true;
    }

    xbridge::App & xapp = xbridge::App::instance();
    WalletConnectorPtr conn = xapp.connectorByCurrency(scurrency);
    if (!conn)
    {
        WARN() << "no connector for <" << scurrency << "> " << __FUNCTION__;
        return true;
    }

    //
    // Check if maker utxos are still valid
    //

    TransactionPtr trPending = e.pendingTransaction(id);
    if (!trPending->matches(id)) {
        WARN() << "no order found with id " << id.ToString() << " " << __FUNCTION__;
        return true;
    }

    WalletConnectorPtr makerConn = xapp.connectorByCurrency(trPending->a_currency());
    if (!makerConn) {
        WARN() << "no maker connector for <" << trPending->a_currency() << "> " << __FUNCTION__;
        return true;
    }

    auto & makerUtxos = trPending->a_utxos();
    for (auto entry : makerUtxos) {
        if (!makerConn->getTxOut(entry)) {
            // Invalid utxos cancel order
            ERR() << "bad maker utxo in order " << id.ToString() << " , utxo txid " << entry.txId << " vout " << entry.vout
                  << " " << __FUNCTION__;
            sendCancelTransaction(trPending, crBadUtxo);
            return false;
        }
    }

    //
    // END Check if maker utxos are still valid
    //

    double commonAmount = 0;

    // utxo items
    std::vector<wallet::UtxoEntry> utxoItems;
    {
        // array size
        uint32_t utxoItemsCount = *static_cast<uint32_t *>(static_cast<void *>(packet->data()+offset));
        offset += sizeof(uint32_t);

        // items
        for (uint32_t i = 0; i < utxoItemsCount; ++i)
        {
            const static uint32_t utxoItemSize = XBridgePacket::hashSize + sizeof(uint32_t) +
                                                 XBridgePacket::addressSize + XBridgePacket::signatureSize;
            if (packet->size() < offset+utxoItemSize)
            {
                WARN() << "bad packet size while reading utxo items, packet dropped in "
                       << __FUNCTION__;
                return true;
            }

            wallet::UtxoEntry entry;

            std::vector<unsigned char> stxid(packet->data()+offset, packet->data()+offset+XBridgePacket::hashSize);
            uint256 txid(stxid);
            offset += XBridgePacket::hashSize;

            entry.txId = txid.ToString();

            entry.vout = *static_cast<uint32_t *>(static_cast<void *>(packet->data()+offset));
            offset += sizeof(uint32_t);

            entry.rawAddress = std::vector<unsigned char>(packet->data()+offset,
                                                          packet->data()+offset+XBridgePacket::addressSize);
            offset += XBridgePacket::addressSize;

            entry.address = conn->fromXAddr(entry.rawAddress);

            entry.signature = std::vector<unsigned char>(packet->data()+offset,
                                                         packet->data()+offset+XBridgePacket::signatureSize);
            offset += XBridgePacket::signatureSize;

            if (!conn->getTxOut(entry))
            {
                LOG() << "not found utxo entry <" << entry.txId
                      << "> no " << entry.vout << " " << __FUNCTION__;
                continue;
            }

            // check signature
            std::string signature = EncodeBase64(&entry.signature[0], entry.signature.size());
            if (!conn->verifyMessage(entry.address, entry.toString(), signature))
            {
                LOG() << "not valid signature, bad utxo entry <" << entry.txId
                      << "> no " << entry.vout << " " << __FUNCTION__;
                continue;
            }

            commonAmount += entry.amount;

            utxoItems.push_back(entry);
        }
    }

    if (commonAmount * TransactionDescr::COIN < samount)
    {
        LOG() << "order rejected, amount from utxo items <" << commonAmount
              << "> less than required <" << samount << "> " << __FUNCTION__;
        return true;
    }

    // check dust amount
    if (conn->isDustAmount(static_cast<double>(samount) / TransactionDescr::COIN) ||
        conn->isDustAmount(commonAmount - (static_cast<double>(samount) / TransactionDescr::COIN)))
    {
        LOG() << "reject dust amount order " << id.ToString() << " " << __FUNCTION__;
        return true;
    }

    LOG() << "received accepting order " << id.ToString() << std::endl
          << "    from " << HexStr(saddr) << std::endl
          << "             " << scurrency << " : " << samount << std::endl
          << "    to   " << HexStr(daddr) << std::endl
          << "             " << dcurrency << " : " << damount << std::endl;


    if (!e.checkUtxoItems(id, utxoItems))
    {
        LOG() << "error accepting order, utxos are bad "
              << __FUNCTION__;
        return true;
    }

    {
        if (e.acceptTransaction(id, saddr, scurrency, samount, daddr, dcurrency, damount, mpubkey, utxoItems))
        {
            // check transaction state, if trNew - do nothing,
            // if trJoined = send hold to client
            TransactionPtr tr = e.transaction(id);
            if (!tr->matches(id)) { // ignore no matching orders
                WARN() << "accept: no order found with id " << id.ToString() << " " << __FUNCTION__;
                return true;
            }

            if (tr->state() != xbridge::Transaction::trJoined)
            {
                xassert(!"wrong state");
                WARN() << "wrong tx state " << tr->id().ToString()
                       << " state " << tr->state()
                       << " in " << __FUNCTION__;
                return true;
            }
            // Set role 'B' utxos used in the order
            tr->b_setUtxos(utxoItems);

            LOG() << __FUNCTION__ << tr;

            XBridgePacketPtr reply1(new XBridgePacket(xbcTransactionHold));
            reply1->append(m_myid);
            reply1->append(tr->id().begin(), XBridgePacket::hashSize);

            reply1->sign(e.pubKey(), e.privKey());

            sendPacketBroadcast(reply1);
        }
    }

    return true;
}

//******************************************************************************
//******************************************************************************
bool Session::Impl::processTransactionHold(XBridgePacketPtr packet) const
{

    DEBUG_TRACE();

    if (packet->size() != 52)
    {
        ERR() << "incorrect packet size for xbcTransactionHold "
              << "need 52 received " << packet->size() << " "
              << __FUNCTION__;
        return false;
    }

    xbridge::App & xapp = xbridge::App::instance();
    uint32_t offset = 0;

    // servicenode addr
    std::vector<unsigned char> hubAddress(packet->data()+offset,
                                          packet->data()+offset+XBridgePacket::addressSize);
    offset += XBridgePacket::addressSize;

    // read packet data
    std::vector<unsigned char> sid(packet->data()+offset, packet->data()+offset+XBridgePacket::hashSize);
    uint256 id(sid);
    offset += XBridgePacket::hashSize;

    // pubkey from packet
    std::vector<unsigned char> spubkey(packet->pubkey(), packet->pubkey()+XBridgePacket::pubkeySize);

    TransactionDescrPtr xtx = xapp.transaction(id);
    if (!xtx)
    {
        LOG() << "unknown order " << id.GetHex() << " " << __FUNCTION__;
        return true;
    }
    // Reject if snode key doesn't match original (prevent order manipulation)
    if (!packet->verify(xtx->sPubKey))
    {
        WARN() << "wrong servicenode handling order, expected " << HexStr(xtx->sPubKey)
               << " but received pubkey " << HexStr(spubkey)
               << " and hub address " << HexStr(hubAddress) << " " << __FUNCTION__;
        return true;
    }

    // Make sure that servicenode is still valid and in the snode list
    CPubKey pksnode;
    pksnode.Set(packet->pubkey(), packet->pubkey() + CPubKey::COMPRESSED_PUBLIC_KEY_SIZE);
    if (!pksnode.IsFullyValid()) {
        LOG() << "Bad Servicenode public key " << __FUNCTION__;
        return false;
    }

    // check servicenode
    auto snode = sn::ServiceNodeMgr::instance().getSn(pksnode);
    if (snode.isNull()) {
        // try to uncompress pubkey and search
        if (pksnode.Decompress())
            snode = sn::ServiceNodeMgr::instance().getSn(pksnode);
        if (snode.isNull()) {
            // bad service node, no more
            LOG() << "unknown service node " << HexStr(pksnode) << " " << __FUNCTION__;
            return true;
        }
    }

    LOG() << "use service node " << HexStr(pksnode) << " " << __FUNCTION__;

    {
        // for xchange node remove tx
        // TODO mark as finished for debug
        Exchange & e = Exchange::instance();
        if (e.isStarted())
        {
            TransactionPtr tr = e.transaction(id);
            if (!tr->matches(id)) // ignore no matching orders
                return true;

            LOG() << __FUNCTION__ << tr;

            if (tr->state() != xbridge::Transaction::trJoined)
            {
                e.deletePendingTransaction(id);
            }

            return true;
        }
    }

    if (xtx->state >= TransactionDescr::trHold)
    {
        xassert(!"wrong state");
        WARN() << "wrong tx state " << xtx->id.ToString()
               << " state " << xtx->state
               << " in " << __FUNCTION__;
        return true;
    }

    if (!xtx->isLocal())
    {
        xtx->state = TransactionDescr::trFinished;

        LOG() << "tx moving to history " << xtx->id.ToString() << " " << __FUNCTION__;

        xapp.moveTransactionToHistory(id);
        xuiConnector.NotifyXBridgeTransactionChanged(xtx->id);
        return true;
    }

    // processing

    WalletConnectorPtr conn = xapp.connectorByCurrency(xtx->toCurrency);
    if (!conn)
    {
        WARN() << "no connector for <" << xtx->toCurrency << "> " << __FUNCTION__;
        return true;
    }

    xtx->state = TransactionDescr::trHold;

    LOG() << __FUNCTION__ << std::endl << "holding order" << xtx;

    xuiConnector.NotifyXBridgeTransactionChanged(id);


    // send hold apply
    XBridgePacketPtr reply(new XBridgePacket(xbcTransactionHoldApply));
    reply->append(hubAddress);
    reply->append(xtx->from);
    reply->append(id.begin(), 32);

    reply->sign(xtx->mPubKey, xtx->mPrivKey);

    sendPacket(hubAddress, reply);

    return true;
}

//*****************************************************************************
//*****************************************************************************
bool Session::Impl::processTransactionHoldApply(XBridgePacketPtr packet) const
{

    DEBUG_TRACE();

    // size must be eq 72 bytes
    if (packet->size() != 72 )
    {
        ERR() << "invalid packet size for xbcTransactionHoldApply "
              << "need 72 received " << packet->size() << " "
              << __FUNCTION__;
        return false;
    }

    // check is for me
    if (!checkPacketAddress(packet))
    {
        return true;
    }

    Exchange & e = Exchange::instance();
    if (!e.isStarted())
    {
        return true;
    }

    uint32_t offset = XBridgePacket::addressSize;

    std::vector<unsigned char> from(packet->data()+offset,
                                    packet->data()+offset+XBridgePacket::addressSize);
    offset += XBridgePacket::addressSize;

    // transaction id
    std::vector<unsigned char> sid(packet->data()+offset, packet->data()+offset+XBridgePacket::hashSize);
    uint256 id(sid);
    // packet pubkey
    std::vector<unsigned char> pubkey(packet->pubkey(), packet->pubkey()+XBridgePacket::pubkeySize);

    TransactionPtr tr = e.transaction(id);
    if (!tr->matches(id)) // ignore no matching orders
        return true;

    if (!packet->verify(tr->a_pk1()) && !packet->verify(tr->b_pk1()))
    {
        WARN() << "bad trader packet signature, received " << HexStr(pubkey)
               << " expected " << HexStr(tr->a_pk1()) << " or " << HexStr(tr->b_pk1()) << " "
               << __FUNCTION__;
        return true;
    }

    if (tr->state() != xbridge::Transaction::trJoined)
    {
        xassert(!"wrong state");
        WARN() << "wrong tx state " << tr->id().ToString()
               << " state " << tr->state()
               << " in " << __FUNCTION__;
        return true;
    }

    tr->updateTimestamp();

    if (!isAddressInTransaction(from, tr))
    {
        ERR() << "invalid transaction address " << __FUNCTION__;
        sendCancelTransaction(tr, crInvalidAddress);
        return true;
    }

    if (e.updateTransactionWhenHoldApplyReceived(tr, from))
    {
        if (tr->state() == xbridge::Transaction::trHold)
        {
            // send initialize transaction command to clients

            // field length must be 8 bytes
            std::vector<unsigned char> a_currency(8, 0);
            std::vector<unsigned char> b_currency(8, 0);
            {
                std::string tmp = tr->a_currency();
                std::copy(tmp.begin(), tmp.end(), a_currency.begin());
                tmp = tr->b_currency();
                std::copy(tmp.begin(), tmp.end(), b_currency.begin());
            }

            // Maker
            XBridgePacketPtr reply1(new XBridgePacket(xbcTransactionInit));
            reply1->append(tr->a_destination());
            reply1->append(m_myid);
            reply1->append(id.begin(), XBridgePacket::hashSize);
            reply1->append(tr->a_address());
            reply1->append(a_currency);
            reply1->append(tr->a_amount());
            reply1->append(tr->a_destination());
            reply1->append(b_currency);
            reply1->append(tr->b_amount());

            reply1->sign(e.pubKey(), e.privKey());

            sendPacket(tr->a_destination(), reply1);

            // Taker
            XBridgePacketPtr reply2(new XBridgePacket(xbcTransactionInit));
            reply2->append(tr->b_destination());
            reply2->append(m_myid);
            reply2->append(id.begin(), XBridgePacket::hashSize);
            reply2->append(tr->b_address());
            reply2->append(b_currency);
            reply2->append(tr->b_amount());
            reply2->append(tr->b_destination());
            reply2->append(a_currency);
            reply2->append(tr->a_amount());

            reply2->sign(e.pubKey(), e.privKey());

            sendPacket(tr->b_destination(), reply2);
        }
    }

    return true;
}

//******************************************************************************
//******************************************************************************
bool Session::Impl::processTransactionInit(XBridgePacketPtr packet) const
{
    DEBUG_TRACE();

    if (packet->size() != 144)
    {
        ERR() << "incorrect packet size for xbcTransactionInit "
              << "need 144 bytes, received " << packet->size() << " "
              << __FUNCTION__;
        return false;
    }

    xbridge::App & xapp = xbridge::App::instance();
    uint32_t offset = 0;

    std::vector<unsigned char> thisAddress(packet->data(),
                                           packet->data()+XBridgePacket::addressSize);
    offset += XBridgePacket::addressSize;

    std::vector<unsigned char> hubAddress(packet->data()+offset,
                                          packet->data()+offset+XBridgePacket::addressSize);
    offset += XBridgePacket::addressSize;

    std::vector<unsigned char> stxid(packet->data()+offset, packet->data()+offset+XBridgePacket::hashSize);
    uint256 txid(stxid);
    offset += XBridgePacket::hashSize;

    TransactionDescrPtr xtx = xapp.transaction(txid);
    if (!xtx)
    {
        LOG() << "unknown order " << txid.ToString() << " " << __FUNCTION__;
        return true;
    }
    if (!xtx->isLocal())
    {
        ERR() << "not a local order " << txid.ToString() << " " << __FUNCTION__;
        return true;
    }
    // Reject if snode key doesn't match original (prevent order manipulation)
    std::vector<unsigned char> spubkey(packet->pubkey(), packet->pubkey()+XBridgePacket::pubkeySize);
    if (!packet->verify(xtx->sPubKey))
    {
        WARN() << "wrong servicenode handling order, expected " << HexStr(xtx->sPubKey)
               << " but received pubkey " << HexStr(spubkey)
               << " and hub address " << HexStr(hubAddress) << " " << __FUNCTION__;
        return true;
    }
    if (xtx->state >= TransactionDescr::trInitialized)
    {
        xassert(!"wrong state");
        WARN() << "wrong tx state " << xtx->id.ToString()
               << " state " << xtx->state
               << " in " << __FUNCTION__;
        return true;
    }

    std::vector<unsigned char> from(packet->data()+offset,
                                    packet->data()+offset+XBridgePacket::addressSize);
    offset += XBridgePacket::addressSize;
    std::string   fromCurrency(reinterpret_cast<const char *>(packet->data()+offset));
    offset += 8;
    uint64_t      fromAmount(*reinterpret_cast<uint64_t *>(packet->data()+offset));
    offset += sizeof(uint64_t);

    std::vector<unsigned char> to(packet->data()+offset,
                                  packet->data()+offset+XBridgePacket::addressSize);
    offset += XBridgePacket::addressSize;
    std::string   toCurrency(reinterpret_cast<const char *>(packet->data()+offset));
    offset += 8;
    uint64_t      toAmount(*reinterpret_cast<uint64_t *>(packet->data()+offset));

    if(xtx->id           != txid &&
       xtx->from         != from &&
       xtx->fromCurrency != fromCurrency &&
       xtx->fromAmount   != fromAmount &&
       xtx->to           != to &&
       xtx->toCurrency   != toCurrency &&
       xtx->toAmount     != toAmount)
    {
        LOG() << "order doesn't match " << __FUNCTION__;
        return true;
    }

    // acceptor fee
    uint256 feetxtd;
    if (xtx->role == 'B')
    {
        // tx with acceptor fee
        WalletConnectorPtr conn = xapp.connectorByCurrency(xtx->toCurrency);
        if (!conn)
        {
            WARN() << "no connector for <" << xtx->toCurrency << "> " << __FUNCTION__;
            return true;
        }

        std::string strtxid;
        if (!rpc::storeDataIntoBlockchain(xtx->rawFeeTx, strtxid))
        {
            ERR() << "storeDataIntoBlockchain failed, error send blocknet tx " << __FUNCTION__;
            sendCancelTransaction(xtx, crBlocknetError);
            return true;
        }

        feetxtd = uint256S(strtxid);

        if(feetxtd.IsNull())
        {
            LOG() << "storeDataIntoBlockchain failed with zero tx id, process packet later " << __FUNCTION__;
            xapp.processLater(txid, packet);
            return true;
        }

        // Unlock fee utxos after fee has been sent to the network
        xapp.unlockFeeUtxos(xtx->feeUtxos);
    }

    xtx->state = TransactionDescr::trInitialized;
    xuiConnector.NotifyXBridgeTransactionChanged(xtx->id);

    // send initialized
    XBridgePacketPtr reply(new XBridgePacket(xbcTransactionInitialized));
    reply->append(hubAddress);
    reply->append(thisAddress);
    reply->append(txid.begin(), 32);
    reply->append(feetxtd.begin(), 32);

    reply->sign(xtx->mPubKey, xtx->mPrivKey);

    sendPacket(hubAddress, reply);

    return true;
}

//*****************************************************************************
//*****************************************************************************
bool Session::Impl::processTransactionInitialized(XBridgePacketPtr packet) const
{
    DEBUG_TRACE();

    // size must be eq 104 bytes
    if (packet->size() != 104)
    {
        ERR() << "invalid packet size for xbcTransactionInitialized "
              << "need 104 received " << packet->size() << " "
              << __FUNCTION__;
        return false;
    }

    // check is for me
    if (!checkPacketAddress(packet))
    {
        return true;
    }

    Exchange & e = Exchange::instance();
    if (!e.isStarted())
    {
        return true;
    }

    uint32_t offset{XBridgePacket::addressSize};

    std::vector<unsigned char> from(packet->data()+offset, packet->data()+offset+XBridgePacket::addressSize);
    offset += XBridgePacket::addressSize;

    // transaction id
    std::vector<unsigned char> sid(packet->data()+offset, packet->data()+offset+XBridgePacket::hashSize);
    uint256 id(sid);

    // opponent public key
    std::vector<unsigned char> pk1(packet->pubkey(), packet->pubkey()+XBridgePacket::pubkeySize);

    // TODO check fee transaction

    TransactionPtr tr = e.transaction(id);
    if (!tr->matches(id)) // ignore no matching orders
        return true;
    
    if (!packet->verify(tr->a_pk1()) && !packet->verify(tr->b_pk1()))
    {
        WARN() << "bad trader packet signature, received " << HexStr(pk1)
               << " expected " << HexStr(tr->a_pk1()) << " or " << HexStr(tr->b_pk1()) << " "
               << __FUNCTION__;
        return true;
    }

    if (tr->state() != xbridge::Transaction::trHold)
    {
        xassert(!"wrong state");
        WARN() << "wrong tx state " << tr->id().ToString()
               << " state " << tr->state()
               << " in " << __FUNCTION__;
        return true;
    }

    tr->updateTimestamp();

    if (!isAddressInTransaction(from, tr))
    {
        ERR() << "invalid transaction address " << __FUNCTION__;
        sendCancelTransaction(tr, crInvalidAddress);
        return true;
    }

    if (e.updateTransactionWhenInitializedReceived(tr, from, pk1))
    {
        if (tr->state() == xbridge::Transaction::trInitialized)
        {
            // send create transaction command to clients

            // Send to Maker
            XBridgePacketPtr reply1(new XBridgePacket(xbcTransactionCreateA));
            reply1->append(m_myid);
            reply1->append(id.begin(), 32);
            reply1->append(tr->b_pk1());

            reply1->sign(e.pubKey(), e.privKey());

            sendPacket(tr->a_address(), reply1);
        }
    }

    LOG() << __FUNCTION__ << tr;

    return true;
}

//******************************************************************************
//******************************************************************************
bool Session::Impl::isAddressInTransaction(const std::vector<unsigned char> & address,
                                           const TransactionPtr & tx) const
{
    if (tx->a_address() == address ||
        tx->b_address() == address ||
        tx->a_destination() == address ||
        tx->b_destination() == address)
    {
        return true;
    }
    return false;
}

//******************************************************************************
//******************************************************************************
bool Session::Impl::processTransactionCreateA(XBridgePacketPtr packet) const
{
    DEBUG_TRACE();

    if (packet->size() != 85)
    {
        ERR() << "incorrect packet size for xbcTransactionCreateA "
              << "need 85 bytes, received " << packet->size() << " "
              << __FUNCTION__;
        return false;
    }

    std::vector<unsigned char> hubAddress(packet->data(), packet->data()+XBridgePacket::addressSize);
    uint32_t offset = XBridgePacket::addressSize;

    // transaction id
    std::vector<unsigned char> stxid(packet->data()+offset, packet->data()+offset+XBridgePacket::hashSize);
    uint256 txid(stxid);
    offset += XBridgePacket::hashSize;

    // counterparty pubkey
    std::vector<unsigned char> mPubKey(packet->data()+offset, packet->data()+offset+XBridgePacket::pubkeySize);

    xbridge::App & xapp = xbridge::App::instance();

    TransactionDescrPtr xtx = xapp.transaction(txid);
    if (!xtx)
    {
        LOG() << "unknown order " << txid.GetHex() << " " << __FUNCTION__;
        return true;
    }
    if (!xtx->isLocal())
    {
        ERR() << "not a local order " << txid.GetHex() << " " << __FUNCTION__;
        return true;
    }
    // Reject if snode key doesn't match original (prevent order manipulation)
    std::vector<unsigned char> spubkey(packet->pubkey(), packet->pubkey()+XBridgePacket::pubkeySize);
    if (!packet->verify(xtx->sPubKey))
    {
        WARN() << "wrong servicenode handling order, expected " << HexStr(xtx->sPubKey)
               << " but received pubkey " << HexStr(spubkey)
               << " and hub address " << HexStr(hubAddress) << " " << __FUNCTION__;
        return true;
    }
    if (xtx->role != 'A')
    {
        ERR() << "received packet for wrong role, expected role A " << __FUNCTION__;
        return true;
    }

    if (xtx->state >= TransactionDescr::trCreated)
    {
        xassert(!"wrong state");
        WARN() << "wrong tx state " << xtx->id.ToString()
               << " state " << xtx->state
               << " in " << __FUNCTION__;
        return true;
    }

    // connectors
    WalletConnectorPtr connFrom = xapp.connectorByCurrency(xtx->fromCurrency);
    WalletConnectorPtr connTo   = xapp.connectorByCurrency(xtx->toCurrency);
    if (!connFrom || !connTo)
    {
        WARN() << "no connector for <" << (!connFrom ? xtx->fromCurrency : xtx->toCurrency) << "> " << __FUNCTION__;
        sendCancelTransaction(xtx, crRpcError);
        return true;
    }

    double outAmount = static_cast<double>(xtx->fromAmount) / TransactionDescr::COIN;

    double fee1      = 0;
    double fee2      = connFrom->minTxFee2(1, 1);
    double inAmount  = 0;

    std::vector<wallet::UtxoEntry> usedInTx;
    for (const wallet::UtxoEntry & entry : xtx->usedCoins)
    {
        usedInTx.push_back(entry);
        inAmount += entry.amount;
        fee1 = connFrom->minTxFee1(usedInTx.size(), 3);

        LOG() << "using utxo item, id: <" << entry.txId << "> amount: " << entry.amount << " vout: " << entry.vout;

        // check amount
        if (inAmount >= outAmount+fee1+fee2)
        {
            break;
        }
    }

    LOG() << "fee1: " << fee1;
    LOG() << "fee2: " << fee2;
    LOG() << "amount of used utxo items: " << inAmount << " required amount + fees: " << outAmount + fee1 + fee2;

    // check amount
    if (inAmount < outAmount+fee1+fee2)
    {
        // no money, cancel transaction
        LOG() << "insufficient funds, canceling order " << __FUNCTION__;
        sendCancelTransaction(xtx, crNoMoney);
        return true;
    }

    // lock time
    xtx->lockTime         = connFrom->lockTime(xtx->role);
    xtx->opponentLockTime = connTo->lockTime('B');
    if (xtx->lockTime == 0 || xtx->opponentLockTime == 0)
    {
        LOG() << "lockTime error, canceling order " << __FUNCTION__;
        sendCancelTransaction(xtx, crRpcError);
        return true;
    }

    // store opponent public key (packet verification)
    xtx->oPubKey = mPubKey;

    // create transactions

    // hash of secret
    std::vector<unsigned char> hx = connTo->getKeyId(xtx->xPubKey);

#ifdef LOG_KEYPAIR_VALUES
    LOG() << "unlock script pub keys" << std::endl <<
             "    my       " << HexStr(xtx->mPubKey) << std::endl <<
             "    my id    " << HexStr(connFrom->getKeyId(xtx->mPubKey)) << std::endl <<
             "    other    " << HexStr(mPubKey) << std::endl <<
             "    other id " << HexStr(connFrom->getKeyId(mPubKey)) << std::endl <<
             "    x id     " << HexStr(hx);
#endif

    // create address for deposit
    connFrom->createDepositUnlockScript(xtx->mPubKey, xtx->oPubKey, hx, xtx->lockTime, xtx->lockScript);
    xtx->lockP2SHAddress = connFrom->scriptIdToString(connFrom->getScriptId(xtx->lockScript));

    auto fromAddr = connFrom->fromXAddr(xtx->from);
    auto toAddr = connTo->fromXAddr(xtx->to);

    // depositTx
    {
        std::vector<xbridge::XTxIn>                  inputs;
        std::vector<std::pair<std::string, double> > outputs;

        // inputs
        wallet::UtxoEntry largestUtxo;
        for (const wallet::UtxoEntry & entry : usedInTx)
        {
            if (entry.amount > largestUtxo.amount)
                largestUtxo = entry;
            inputs.emplace_back(entry.txId, entry.vout, entry.amount);
        }

        // outputs

        // amount
        outputs.push_back(std::make_pair(xtx->lockP2SHAddress, outAmount+fee2));

        // rest
        if (inAmount > outAmount+fee1+fee2)
        {
            double rest = inAmount-outAmount-fee1-fee2;
            outputs.push_back(std::make_pair(largestUtxo.address, rest)); // change back to largest input used in order
        }

        if (!connFrom->createDepositTransaction(inputs, outputs, xtx->binTxId, xtx->binTxVout, xtx->binTx))
        {
            // cancel transaction
            ERR() << "failed to create deposit transaction, canceling order " << __FUNCTION__;
            TXLOG() << "deposit transaction for order " << xtx->id.ToString() << " (submit manually using sendrawtransaction) "
                    << xtx->fromCurrency << "(" << xbridge::xBridgeStringValueFromAmount(xtx->fromAmount) << " - " << fromAddr << ") / "
                    << xtx->toCurrency   << "(" << xbridge::xBridgeStringValueFromAmount(xtx->toAmount)   << " - " << toAddr   << ") "
                    << "using locktime " << xtx->lockTime << std::endl
                    << xtx->binTx;
            sendCancelTransaction(xtx, crRpcError);
            return true;
        }

        TXLOG() << "deposit transaction for order " << xtx->id.ToString() << " (submit manually using sendrawtransaction) "
                << xtx->fromCurrency << "(" << xbridge::xBridgeStringValueFromAmount(xtx->fromAmount) << " - " << fromAddr << ") / "
                << xtx->toCurrency   << "(" << xbridge::xBridgeStringValueFromAmount(xtx->toAmount)   << " - " << toAddr   << ") "
                << "using locktime " << xtx->lockTime << std::endl
                << xtx->binTx;

    } // depositTx

    // refundTx
    {
        std::vector<xbridge::XTxIn>                  inputs;
        std::vector<std::pair<std::string, double> > outputs;

        // inputs from binTx
        inputs.emplace_back(xtx->binTxId, xtx->binTxVout, outAmount+fee2);

        // outputs
        {
            std::string addr = xtx->refundAddress();
            if (addr.empty()) {
                if (!connFrom->getNewAddress(addr))
                {
                    // cancel order
                    LOG() << "failed to getnewaddress for refund tx, canceling order " << xtx->id.ToString() << " "
                          << __FUNCTION__;
                    sendCancelTransaction(xtx, crRpcError);
                    return true;
                }
            }

            outputs.push_back(std::make_pair(addr, outAmount));
        }

        if (!connFrom->createRefundTransaction(inputs, outputs,
                                               xtx->mPubKey, xtx->mPrivKey,
                                               xtx->lockScript, xtx->lockTime,
                                               xtx->refTxId, xtx->refTx))
        {
            // cancel order
            ERR() << "failed to create refund transaction, canceling order " << __FUNCTION__;
            TXLOG() << "refund transaction for order " << xtx->id.ToString() << " (submit manually using sendrawtransaction) "
                    << xtx->fromCurrency << "(" << xbridge::xBridgeStringValueFromAmount(xtx->fromAmount) << " - " << fromAddr << ") / "
                    << xtx->toCurrency   << "(" << xbridge::xBridgeStringValueFromAmount(xtx->toAmount)   << " - " << toAddr   << ")" << std::endl
                    << xtx->refTx;
            sendCancelTransaction(xtx, crRpcError);
            return true;
        }

        TXLOG() << "refund transaction for order " << xtx->id.ToString() << " (submit manually using sendrawtransaction) "
                << xtx->fromCurrency << "(" << xbridge::xBridgeStringValueFromAmount(xtx->fromAmount) << " - " << fromAddr << ") / "
                << xtx->toCurrency   << "(" << xbridge::xBridgeStringValueFromAmount(xtx->toAmount)   << " - " << toAddr   << ")" << std::endl
                << xtx->refTx;

    } // refundTx

    xtx->state = TransactionDescr::trCreated;
    xuiConnector.NotifyXBridgeTransactionChanged(txid);
    
    // Sending deposit
    xtx->sentDeposit();

    // send transactions
    {
        std::string sentid;
        int32_t errCode = 0;
        std::string errorMessage;
        if (connFrom->sendRawTransaction(xtx->binTx, sentid, errCode, errorMessage))
        {
            LOG() << "successfully deposited in p2sh: txid " << xtx->binTxId << " sent id " << sentid;
        }
        else
        {
            LOG() << "error sending deposit, canceling order " << __FUNCTION__;
            xtx->failDeposit();
            sendCancelTransaction(xtx, crRpcError);
            return true;
        }
    }

    // send reply
    XBridgePacketPtr reply;
    reply.reset(new XBridgePacket(xbcTransactionCreatedA));

    reply->append(hubAddress);
    reply->append(txid.begin(), 32);
    reply->append(xtx->binTxId);
    reply->append(hx);
    reply->append(xtx->lockTime);
    reply->append(xtx->refTxId);
    reply->append(xtx->refTx);

    reply->sign(xtx->mPubKey, xtx->mPrivKey);

    sendPacket(hubAddress, reply);

    return true;
}

//*****************************************************************************
//*****************************************************************************
bool Session::Impl::processTransactionCreatedA(XBridgePacketPtr packet) const
{
    DEBUG_TRACE();

    // size must be > 76 bytes
    if (packet->size() <= 76)
    {
        ERR() << "invalid packet size for xbcTransactionCreatedA "
              << "need more than 76, received " << packet->size() << " "
              << __FUNCTION__;
        return false;
    }

    // check is for me
    if (!checkPacketAddress(packet))
    {
        return true;
    }

    Exchange & e = Exchange::instance();
    if (!e.isStarted())
    {
        return true;
    }

    size_t offset = XBridgePacket::addressSize; // hub address

    std::vector<unsigned char> stxid(packet->data()+offset, packet->data()+offset+XBridgePacket::hashSize);
    uint256 txid(stxid);
    offset += XBridgePacket::hashSize;

    std::string binTxId(reinterpret_cast<const char *>(packet->data()+offset));
    offset += binTxId.size()+1;

    std::vector<unsigned char> hx(packet->data()+offset, packet->data()+offset+XBridgePacket::addressSize);
    offset += XBridgePacket::addressSize;

    uint32_t lockTimeA = *reinterpret_cast<uint32_t *>(packet->data()+offset);
    offset += sizeof(uint32_t);

    std::string refTxId(reinterpret_cast<const char *>(packet->data()+offset));
    offset += refTxId.size()+1;

    std::string refTx(reinterpret_cast<const char *>(packet->data()+offset));

    TransactionPtr tr = e.transaction(txid);
    if (!tr->matches(txid)) // ignore no matching orders
        return true;

    std::vector<unsigned char> pk1(packet->pubkey(), packet->pubkey()+XBridgePacket::pubkeySize);
    if (!packet->verify(tr->a_pk1()))
    {
        WARN() << "bad counterparty packet signature, received " << HexStr(pk1)
               << " expected " << HexStr(tr->a_pk1()) << " " << __FUNCTION__;
        return true;
    }

    if (tr->state() != xbridge::Transaction::trInitialized)
    {
        xassert(!"wrong state");
        WARN() << "wrong tx state " << tr->id().ToString()
               << " state " << tr->state()
               << " in " << __FUNCTION__;
        return true;
    }

    tr->a_setLockTime(lockTimeA);
    tr->a_setRefundTx(refTxId, refTx);
    tr->updateTimestamp();

    xbridge::App & xapp = xbridge::App::instance();
    xapp.watchTraderDeposit(tr);

    if (e.updateTransactionWhenCreatedReceived(tr, tr->a_address(), binTxId))
    {
        ERR() << "bad state detected on order " << tr->id().ToString() << " " << __FUNCTION__;
        return true;
    }

    XBridgePacketPtr reply2(new XBridgePacket(xbcTransactionCreateB));
    reply2->append(m_myid);
    reply2->append(txid.begin(), 32);
    reply2->append(tr->a_pk1());
    reply2->append(binTxId);
    reply2->append(hx);
    reply2->append(lockTimeA);

    reply2->sign(e.pubKey(), e.privKey());

    sendPacket(tr->b_address(), reply2);

    LOG() << __FUNCTION__ << tr;

    return true;
}

//******************************************************************************
//******************************************************************************
bool Session::Impl::processTransactionCreateB(XBridgePacketPtr packet) const
{
    DEBUG_TRACE();

    if (packet->size() <= 109)
    {
        ERR() << "incorrect packet size for xbcTransactionCreateB "
              << "need more than 109 bytes, received " << packet->size() << " "
              << __FUNCTION__;
        return false;
    }

    std::vector<unsigned char> hubAddress(packet->data(), packet->data()+XBridgePacket::addressSize);
    uint32_t offset = XBridgePacket::addressSize;

    // transaction id
    std::vector<unsigned char> stxid(packet->data()+offset, packet->data()+offset+XBridgePacket::hashSize);
    uint256 txid(stxid);
    offset += XBridgePacket::hashSize;

    std::vector<unsigned char> mPubKey(packet->data()+offset, packet->data()+offset+XBridgePacket::pubkeySize);
    offset += XBridgePacket::pubkeySize;

    std::string binATxId(reinterpret_cast<const char *>(packet->data()+offset));
    offset += binATxId.size()+1;

    std::vector<unsigned char> hx(packet->data()+offset, packet->data()+offset+XBridgePacket::addressSize);
    offset += XBridgePacket::addressSize;

    uint32_t lockTimeA = *reinterpret_cast<uint32_t *>(packet->data()+offset);

    xbridge::App & xapp = xbridge::App::instance();

    TransactionDescrPtr xtx = xapp.transaction(txid);
    if (!xtx)
    {
        LOG() << "unknown order " << txid.GetHex() << " " << __FUNCTION__;
        return true;
    }
    if (!xtx->isLocal())
    {
        ERR() << "not a local order " << txid.GetHex() << " " << __FUNCTION__;
        return true;
    }
    // Reject if snode key doesn't match original (prevent order manipulation)
    std::vector<unsigned char> spubkey(packet->pubkey(), packet->pubkey()+XBridgePacket::pubkeySize);
    if (!packet->verify(xtx->sPubKey))
    {
        WARN() << "wrong servicenode handling order, expected " << HexStr(xtx->sPubKey)
               << " but received pubkey " << HexStr(spubkey)
               << " and hub address " << HexStr(hubAddress) << " " << __FUNCTION__;
        return true;
    }
    if (xtx->state >= TransactionDescr::trCreated)
    {
        xassert(!"wrong state");
        WARN() << "wrong tx state " << xtx->id.ToString()
               << " state " << xtx->state
               << " in " << __FUNCTION__;
        return true;
    }
    if (binATxId.size() == 0)
    {
        LOG() << "bad counterparty deposit tx id received for order " << txid.GetHex() << " " << __FUNCTION__;
        sendCancelTransaction(xtx, crBadADepositTx);
        return true;
    }
    if (xtx->role != 'B')
    {
        ERR() << "received packet for wrong role, expected role B " << __FUNCTION__;
        return true;
    }
    if(xtx->xPubKey.size() != 0)
    {
        ERR() << "bad role" << __FUNCTION__;
        return true;
    }

    // connectors
    WalletConnectorPtr connFrom = xapp.connectorByCurrency(xtx->fromCurrency);
    WalletConnectorPtr connTo   = xapp.connectorByCurrency(xtx->toCurrency);
    if (!connFrom || !connTo)
    {
        WARN() << "no connector for <" << (!connFrom ? xtx->fromCurrency : xtx->toCurrency) << "> " << __FUNCTION__;
        sendCancelTransaction(xtx, crRpcError);
        return true;
    }

    double outAmount = static_cast<double>(xtx->fromAmount) / TransactionDescr::COIN;
    double checkAmount = static_cast<double>(xtx->toAmount) / TransactionDescr::COIN;

    // check preliminary lock times for counterparty
    {
        if (lockTimeA == 0 || !connTo->acceptableLockTimeDrift('A', lockTimeA))
        {
            LOG() << "incorrect locktime " << lockTimeA << " from counterparty on order " << txid.GetHex() << " "
                  << "expected " << connTo->lockTime('A') << " "
                  << __FUNCTION__;
            sendCancelTransaction(xtx, crBadADepositTx);
            return true;
        }
    }

    // Counterparty hashed secret
    xtx->oHashedSecret    = hx;
    // Set lock times
    xtx->lockTime         = connFrom->lockTime('B'); // expected locktime for trader B (me)
    xtx->opponentLockTime = lockTimeA;

    // Generate counterparty script
    std::vector<unsigned char> counterPartyScript;
    connTo->createDepositUnlockScript(mPubKey, xtx->mPubKey, xtx->oHashedSecret, xtx->opponentLockTime, counterPartyScript);
    std::string counterPartyP2SH = connTo->scriptIdToString(connTo->getScriptId(counterPartyScript));

    // Counter party voutN
    uint32_t counterPartyVoutN = 0;

    // check A deposit tx and check that counterparty script is valid in counterparty deposit tx
    {
        bool isGood = false;
        if (!connTo->checkDepositTransaction(binATxId, std::string(), checkAmount, counterPartyVoutN, counterPartyP2SH, xtx->oOverpayment, isGood))
        {
            // move packet to pending
            xapp.processLater(txid, packet);
            return true;
        }
        else if (!isGood)
        {
            LOG() << "bad counterparty deposit for order " << txid.GetHex() << " , canceling order " << __FUNCTION__;
            sendCancelTransaction(xtx, crBadADepositTx);
            return true;
        }

        LOG() << "counterparty deposit confirmed for order " << txid.GetHex();
    }

    double fee1      = 0;
    double fee2      = connFrom->minTxFee2(1, 1);
    double inAmount  = 0;

    std::vector<wallet::UtxoEntry> usedInTx;
    for (const wallet::UtxoEntry & entry : xtx->usedCoins)
    {
        usedInTx.push_back(entry);
        inAmount += entry.amount;
        fee1 = connFrom->minTxFee1(usedInTx.size(), 3);

        LOG() << "using utxo item, id: <" << entry.txId << "> amount: " << entry.amount << " vout: " << entry.vout;

        // check amount
        if (inAmount >= outAmount+fee1+fee2)
        {
            break;
        }
    }

    LOG() << "fee1: " << fee1;
    LOG() << "fee2: " << fee2;
    LOG() << "amount of used utxo items: " << inAmount << " required amount + fees: " << outAmount + fee1 + fee2;

    // check amount
    if (inAmount < outAmount+fee1+fee2)
    {
        // no money, cancel transaction
        LOG() << "insufficient funds, canceling order " << __FUNCTION__;
        sendCancelTransaction(xtx, crNoMoney);
        return true;
    }

    // store counterparty public key (packet verification)
    xtx->oPubKey = mPubKey;
    // store counterparty tx info
    xtx->oBinTxId = binATxId;
    xtx->oBinTxVout = counterPartyVoutN;
    // store counterparty script
    xtx->unlockScript = counterPartyScript;
    xtx->unlockP2SHAddress = counterPartyP2SH;

    // create transactions

#ifdef LOG_KEYPAIR_VALUES
    LOG() << "unlock script pub keys" << std::endl <<
             "    my       " << HexStr(xtx->mPubKey) << std::endl <<
             "    my id    " << HexStr(connFrom->getKeyId(xtx->mPubKey)) << std::endl <<
             "    other    " << HexStr(xtx->oPubKey) << std::endl <<
             "    other id " << HexStr(connFrom->getKeyId(xtx->oPubKey)) << std::endl <<
             "    x id     " << HexStr(xtx->oHashedSecret);
#endif

    // create address for deposit
    connFrom->createDepositUnlockScript(xtx->mPubKey, xtx->oPubKey, xtx->oHashedSecret, xtx->lockTime, xtx->lockScript);
    xtx->lockP2SHAddress = connFrom->scriptIdToString(connFrom->getScriptId(xtx->lockScript));

    auto fromAddr = connFrom->fromXAddr(xtx->from);
    auto toAddr = connTo->fromXAddr(xtx->to);

    // depositTx
    {
        std::vector<xbridge::XTxIn>                  inputs;
        std::vector<std::pair<std::string, double> > outputs;

        // inputs
        wallet::UtxoEntry largestUtxo;
        for (const wallet::UtxoEntry & entry : usedInTx)
        {
            if (entry.amount > largestUtxo.amount)
                largestUtxo = entry;
            inputs.emplace_back(entry.txId, entry.vout, entry.amount);
        }

        // outputs

        // amount
        outputs.push_back(std::make_pair(xtx->lockP2SHAddress, outAmount+fee2));

        // rest
        if (inAmount > outAmount+fee1+fee2)
        {
            double rest = inAmount-outAmount-fee1-fee2;
            outputs.push_back(std::make_pair(largestUtxo.address, rest)); // change back to largest input used in order
        }

        if (!connFrom->createDepositTransaction(inputs, outputs, xtx->binTxId, xtx->binTxVout, xtx->binTx))
        {
            // cancel transaction
            ERR() << "failed to create deposit transaction, canceling order " << __FUNCTION__;
            TXLOG() << "deposit transaction for order " << xtx->id.ToString() << " (submit manually using sendrawtransaction) "
                    << xtx->fromCurrency << "(" << xbridge::xBridgeStringValueFromAmount(xtx->fromAmount) << " - " << fromAddr << ") / "
                    << xtx->toCurrency   << "(" << xbridge::xBridgeStringValueFromAmount(xtx->toAmount)   << " - " << toAddr   << ") "
                    << "using locktime " << xtx->lockTime << std::endl
                    << xtx->binTx;
            sendCancelTransaction(xtx, crRpcError);
            return true;
        }

        TXLOG() << "deposit transaction for order " << xtx->id.ToString() << " (submit manually using sendrawtransaction) "
                << xtx->fromCurrency << "(" << xbridge::xBridgeStringValueFromAmount(xtx->fromAmount) << " - " << fromAddr << ") / "
                << xtx->toCurrency   << "(" << xbridge::xBridgeStringValueFromAmount(xtx->toAmount)   << " - " << toAddr   << ") "
                << "using locktime " << xtx->lockTime << std::endl
                << xtx->binTx;

    } // depositTx

    // refundTx
    {
        std::vector<xbridge::XTxIn>                  inputs;
        std::vector<std::pair<std::string, double> > outputs;

        // inputs from binTx
        inputs.emplace_back(xtx->binTxId, xtx->binTxVout, outAmount+fee2);

        // outputs
        {
            std::string addr = xtx->refundAddress();
            if (addr.empty()) {
                if (!connFrom->getNewAddress(addr)) {
                    // cancel order
                    LOG() << "failed to getnewaddress for refund tx, canceling order " << xtx->id.ToString() << " "
                          << __FUNCTION__;
                    sendCancelTransaction(xtx, crRpcError);
                    return true;
                }
            }

            outputs.push_back(std::make_pair(addr, outAmount));
        }

        if (!connFrom->createRefundTransaction(inputs, outputs,
                                               xtx->mPubKey, xtx->mPrivKey,
                                               xtx->lockScript, xtx->lockTime,
                                               xtx->refTxId, xtx->refTx))
        {
            // cancel order
            ERR() << "failed to create refund transaction, canceling order " << __FUNCTION__;
            TXLOG() << "refund transaction for order " << xtx->id.ToString() << " "
                    << xtx->fromCurrency << "(" << xbridge::xBridgeStringValueFromAmount(xtx->fromAmount) << " - " << fromAddr << ") / "
                    << xtx->toCurrency   << "(" << xbridge::xBridgeStringValueFromAmount(xtx->toAmount)   << " - " << toAddr   << ")" << std::endl
                    << xtx->refTx;
            sendCancelTransaction(xtx, crRpcError);
            return true;
        }

        TXLOG() << "refund transaction for order " << xtx->id.ToString() << " "
                << xtx->fromCurrency << "(" << xbridge::xBridgeStringValueFromAmount(xtx->fromAmount) << " - " << fromAddr << ") / "
                << xtx->toCurrency   << "(" << xbridge::xBridgeStringValueFromAmount(xtx->toAmount)   << " - " << toAddr   << ")" << std::endl
                << xtx->refTx;

    } // refundTx


    // send transactions
    {
        // Get the current block
        rpc::WalletInfo info;
        if (!connFrom->getInfo(info)) {
            ERR() << "failed to obtain block count from " << xtx->fromCurrency << " blockchain "
                  << __FUNCTION__;
            sendCancelTransaction(xtx, crRpcError);
            return true;
        }
        
        xtx->state = TransactionDescr::trCreated;
        xuiConnector.NotifyXBridgeTransactionChanged(txid);

        // Mark deposit as sent
        xtx->sentDeposit();
        
        std::string sentid;
        int32_t errCode = 0;
        std::string errorMessage;
        if (connFrom->sendRawTransaction(xtx->binTx, sentid, errCode, errorMessage))
        {
            LOG() << "successfully deposited in p2sh: txid " << xtx->binTxId << " sent id " << sentid;
            xtx->setWatchBlock(info.blocks);
            xapp.watchForSpentDeposit(xtx);
        }
        else
        {
            LOG() << "error sending deposit tx, canceling order " << __FUNCTION__;
            xtx->failDeposit();
            sendCancelTransaction(xtx, crRpcError);
            return true;
        }
    }

    // send reply
    XBridgePacketPtr reply;
    reply.reset(new XBridgePacket(xbcTransactionCreatedB));

    reply->append(hubAddress);
    reply->append(txid.begin(), 32);
    reply->append(xtx->binTxId);
    reply->append(xtx->lockTime);
    reply->append(xtx->refTxId);
    reply->append(xtx->refTx);

    reply->sign(xtx->mPubKey, xtx->mPrivKey);
    sendPacket(hubAddress, reply);

    return true;
}

//*****************************************************************************
//*****************************************************************************
bool Session::Impl::processTransactionCreatedB(XBridgePacketPtr packet) const
{
    DEBUG_TRACE();

    // size must be > 56 bytes
    if (packet->size() <= 56)
    {
        ERR() << "invalid packet size for xbcTransactionCreated "
              << "need more than 56 bytes, received " << packet->size() << " "
              << __FUNCTION__;
        return false;
    }

    // check is for me
    if (!checkPacketAddress(packet))
    {
        return true;
    }

    Exchange & e = Exchange::instance();
    if (!e.isStarted())
    {
        return true;
    }

    size_t offset = XBridgePacket::addressSize; // hub address

    std::vector<unsigned char> stxid(packet->data()+offset, packet->data()+offset+XBridgePacket::hashSize);
    uint256 txid(stxid);
    offset += XBridgePacket::hashSize;

    std::string binTxId(reinterpret_cast<const char *>(packet->data()+offset));
    offset += binTxId.size()+1;

    uint32_t lockTimeB = *reinterpret_cast<uint32_t *>(packet->data()+offset);
    offset += sizeof(uint32_t);

    std::string refTxId(reinterpret_cast<const char *>(packet->data()+offset));
    offset += refTxId.size()+1;

    std::string refTx(reinterpret_cast<const char *>(packet->data()+offset));

    TransactionPtr tr = e.transaction(txid);
    if (!tr->matches(txid)) // ignore no matching orders
        return true;

    std::vector<unsigned char> pk1(packet->pubkey(), packet->pubkey()+XBridgePacket::pubkeySize);
    if (!packet->verify(tr->b_pk1()))
    {
        WARN() << "bad counterparty packet signature, received " << HexStr(pk1)
               << " expected " << HexStr(tr->b_pk1()) << " " << __FUNCTION__;
        return true;
    }

    if (tr->state() != xbridge::Transaction::trInitialized)
    {
        xassert(!"wrong state");
        WARN() << "wrong tx state " << tr->id().ToString()
               << " state " << tr->state()
               << " in " << __FUNCTION__;
        return true;
    }

    tr->b_setLockTime(lockTimeB);
    tr->b_setRefundTx(refTxId, refTx);
    tr->updateTimestamp();

    if (e.updateTransactionWhenCreatedReceived(tr, tr->b_address(), binTxId))
    {
        if (tr->state() == xbridge::Transaction::trCreated)
        {
            // send confirm packets with deposit tx id
            // for create payment tx

            XBridgePacketPtr reply(new XBridgePacket(xbcTransactionConfirmA));
            reply->append(m_myid);
            reply->append(txid.begin(), 32);
            reply->append(tr->b_bintxid());
            reply->append(lockTimeB);

            reply->sign(e.pubKey(), e.privKey());

            sendPacket(tr->a_destination(), reply);
        }
    }

    LOG() << __FUNCTION__ << tr;

    return true;
}

//******************************************************************************
//******************************************************************************
bool Session::Impl::processTransactionConfirmA(XBridgePacketPtr packet) const
{
    DEBUG_TRACE();

    // size must be > 56 bytes
    if (packet->size() <= 56)
    {
        LOG() << "incorrect packet size for xbcTransactionConfirmA "
              << "need more than 56 bytes, received " << packet->size() << " "
              << __FUNCTION__;
        return false;
    }

    std::vector<unsigned char> hubAddress(packet->data(), packet->data()+XBridgePacket::addressSize);
    size_t offset = XBridgePacket::addressSize;

    std::vector<unsigned char> stxid(packet->data()+offset, packet->data()+offset+XBridgePacket::hashSize);
    uint256 txid(stxid);
    offset += XBridgePacket::hashSize;

    std::string binTxId(reinterpret_cast<const char *>(packet->data()+offset));
    offset += binTxId.size()+1;

    uint32_t lockTimeB = *reinterpret_cast<uint32_t *>(packet->data()+offset);

    xbridge::App & xapp = xbridge::App::instance();

    TransactionDescrPtr xtx = xapp.transaction(txid);
    if (!xtx)
    {
        LOG() << "unknown order " << HexStr(txid) << " " << __FUNCTION__;
        return true;
    }
    if (!xtx->isLocal())
    {
        ERR() << "not a local order " << HexStr(txid) << " " << __FUNCTION__;
        return true;
    }
    // Reject if servicenode key doesn't match original (prevent order manipulation)
    std::vector<unsigned char> spubkey(packet->pubkey(), packet->pubkey()+XBridgePacket::pubkeySize);
    if (!packet->verify(xtx->sPubKey))
    {
        WARN() << "wrong servicenode handling order, expected " << HexStr(xtx->sPubKey)
               << " but received pubkey " << HexStr(spubkey)
               << " and hub address " << HexStr(hubAddress) << " " << __FUNCTION__;
        return true;
    }
    if (xtx->state >= TransactionDescr::trCommited)
    {
        xassert(!"wrong state");
        WARN() << "wrong tx state " << xtx->id.ToString()
               << " state " << xtx->state
               << " in " << __FUNCTION__;
        return true;
    }
    if (xtx->role != 'A')
    {
        ERR() << "received packet for wrong role, expected role A " << __FUNCTION__;
        return true;
    }

    // connectors
    WalletConnectorPtr connFrom = xapp.connectorByCurrency(xtx->fromCurrency);
    WalletConnectorPtr connTo   = xapp.connectorByCurrency(xtx->toCurrency);
    if (!connFrom || !connTo)
    {
        WARN() << "no connector for <" << (!connFrom ? xtx->fromCurrency : xtx->toCurrency) << "> " << __FUNCTION__;
        sendCancelTransaction(xtx, crRpcError);
        return true;
    }

    double outAmount   = static_cast<double>(xtx->toAmount)/TransactionDescr::COIN;
    double checkAmount = outAmount;

    // check preliminary lock times for counterparty
    {
        if (lockTimeB == 0 || !connTo->acceptableLockTimeDrift('B', lockTimeB))
        {
            LOG() << "incorrect locktime " << lockTimeB << " from counterparty on order " << txid.GetHex() << " "
                  << "expected " << connTo->lockTime('B') << " "
                  << __FUNCTION__;
            sendCancelTransaction(xtx, crBadBDepositTx);
            return true;
        }
    }

    // Set counterparty lock time
    xtx->opponentLockTime = lockTimeB;

    // Hash of secret
    std::vector<unsigned char> hx = connTo->getKeyId(xtx->xPubKey);

    // Counterparty script
    std::vector<unsigned char> counterPartyScript;
    connTo->createDepositUnlockScript(xtx->oPubKey, xtx->mPubKey, hx, xtx->opponentLockTime, counterPartyScript);
    std::string counterPartyP2SH = connTo->scriptIdToString(connTo->getScriptId(counterPartyScript));

    // Counter party voutN
    uint32_t counterPartyVoutN = 0;

    // check B deposit tx and check that counterparty script is valid in counterparty deposit tx
    {
        bool isGood = false;
        if (!connTo->checkDepositTransaction(binTxId, std::string(), checkAmount, counterPartyVoutN, counterPartyP2SH, xtx->oOverpayment, isGood))
        {
            // move packet to pending
            xapp.processLater(txid, packet);
            return true;
        }
        else if (!isGood)
        {
            LOG() << "bad counterparty deposit for order " << txid.GetHex() << " , canceling order " << __FUNCTION__;
            sendCancelTransaction(xtx, crBadBDepositTx);
            return true;
        }

        LOG() << "counterparty deposit confirmed for order " << txid.GetHex();
    }

    // Set counterparty tx info
    xtx->oBinTxId = binTxId;
    xtx->oBinTxVout = counterPartyVoutN;
    // Set counterparty script
    xtx->unlockScript = counterPartyScript;
    xtx->unlockP2SHAddress = counterPartyP2SH;

    // payTx
    {
        int32_t errCode = 0;
        if (!redeemOrderCounterpartyDeposit(xtx, errCode)) {
            if (errCode == RPCErrorCode::RPC_VERIFY_ERROR) { // missing inputs, wait deposit tx
                LOG() << "trying to redeem again";
                xapp.processLater(txid, packet);
                return true;
            } else {
                LOG() << "failed to redeem tx from counterparty, canceling order";
                sendCancelTransaction(xtx, crRpcError);
                return true;
            }
        }
    } // payTx

    xtx->state = TransactionDescr::trCommited;
    xuiConnector.NotifyXBridgeTransactionChanged(txid);

    // send reply
    XBridgePacketPtr reply(new XBridgePacket(xbcTransactionConfirmedA));
    reply->append(hubAddress);
    reply->append(txid.begin(), 32);
    reply->append(xtx->payTxId);

    reply->sign(xtx->mPubKey, xtx->mPrivKey);

    sendPacket(hubAddress, reply);

    return true;
}

//*****************************************************************************
//*****************************************************************************
bool Session::Impl::processTransactionConfirmedA(XBridgePacketPtr packet) const
{
    DEBUG_TRACE();

    // size must be > 52 bytes
    if (packet->size() <= 52 || packet->size() > 1000)
    {
        ERR() << "invalid packet size for xbcTransactionConfirmedA "
              << "need more than 52 bytes and less than 1kb, received " << packet->size() << " "
              << __FUNCTION__;
        return false;
    }

    // check is for me
    if (!checkPacketAddress(packet))
    {
        return true;
    }

    Exchange & e = Exchange::instance();
    if (!e.isStarted())
    {
        return true;
    }

    size_t offset = XBridgePacket::addressSize; // hub address

    // order id
    std::vector<unsigned char> stxid(packet->data()+offset, packet->data()+offset+XBridgePacket::hashSize);
    uint256 txid(stxid);
    offset += XBridgePacket::hashSize;

    // A side paytx id
    std::string a_payTxId(reinterpret_cast<const char *>(packet->data()+offset));

    TransactionPtr tr = e.transaction(txid);
    if (!tr->matches(txid)) // ignore no matching orders
        return true;

    std::vector<unsigned char> pk1(packet->pubkey(), packet->pubkey()+XBridgePacket::pubkeySize);
    if (!packet->verify(tr->a_pk1()))
    {
        WARN() << "bad counterparty packet signature, received " << HexStr(pk1)
               << " expected " << HexStr(tr->a_pk1()) << " " << __FUNCTION__;
        return true;
    }

    if (tr->state() != xbridge::Transaction::trCreated)
    {
        xassert(!"wrong state");
        WARN() << "wrong tx state " << tr->id().ToString()
               << " state " << tr->state()
               << " in " << __FUNCTION__;
        return true;
    }

    tr->updateTimestamp();
    tr->a_setPayTxId(a_payTxId);

    if (e.updateTransactionWhenConfirmedReceived(tr, tr->a_destination()))
    {
        ERR() << "invalid confirmation " << __FUNCTION__;
        // Can't cancel here, Maker already spent Taker deposit
    }

    XBridgePacketPtr reply2(new XBridgePacket(xbcTransactionConfirmB));
    reply2->append(m_myid);
    reply2->append(txid.begin(), 32);
    reply2->append(tr->a_payTxId());

    reply2->sign(e.pubKey(), e.privKey());

    sendPacket(tr->b_destination(), reply2);

    LOG() << __FUNCTION__ << tr;

    return true;
}

//******************************************************************************
//******************************************************************************
bool Session::Impl::processTransactionConfirmB(XBridgePacketPtr packet) const
{
    DEBUG_TRACE();

    // size must be > 52 bytes
    if (packet->size() <= 52 || packet->size() > 1000)
    {
        LOG() << "incorrect packet size for xbcTransactionConfirmB "
              << "need more than 52 bytes or less than 1kb, received " << packet->size() << " "
              << __FUNCTION__;
        return false;
    }

    std::vector<unsigned char> hubAddress(packet->data(), packet->data()+XBridgePacket::addressSize);
    size_t offset = XBridgePacket::addressSize;

    std::vector<unsigned char> stxid(packet->data()+offset, packet->data()+offset+XBridgePacket::hashSize);
    uint256 txid(stxid);
    offset += XBridgePacket::hashSize;

    std::string payTxId(reinterpret_cast<const char *>(packet->data()+offset));
    offset += payTxId.size()+1;

    xbridge::App & xapp = xbridge::App::instance();

    TransactionDescrPtr xtx = xapp.transaction(txid);
    if (!xtx)
    {
        LOG() << "unknown order " << txid.GetHex() << " " << __FUNCTION__;
        return true;
    }
    if (!xtx->isLocal())
    {
        ERR() << "order is not local " << txid.GetHex() << " " << __FUNCTION__;
        return true;
    }
    // Reject if servicenode key doesn't match original (prevent order manipulation)
    std::vector<unsigned char> spubkey(packet->pubkey(), packet->pubkey()+XBridgePacket::pubkeySize);
    if (!packet->verify(xtx->sPubKey))
    {
        WARN() << "wrong servicenode handling order, expected " << HexStr(xtx->sPubKey)
               << " but received pubkey " << HexStr(spubkey)
               << " and hub address " << HexStr(hubAddress) << " " << __FUNCTION__;
        return true;
    }
    if (xtx->state >= TransactionDescr::trCommited)
    {
        xassert(!"wrong state");
        WARN() << "wrong tx state " << xtx->id.ToString()
               << " state " << xtx->state
               << " in " << __FUNCTION__;
        return true;
    }

    // Only use counterparties payTxId for a few iterations
    if (xtx->otherPayTxTries() < xtx->maxOtherPayTxTries() && !xtx->isDoneWatching()) {
        xtx->setOtherPayTxId(payTxId);
        xtx->tryOtherPayTx();
    }

    WalletConnectorPtr connFrom = xapp.connectorByCurrency(xtx->fromCurrency);
    WalletConnectorPtr connTo = xapp.connectorByCurrency(xtx->toCurrency);
    if (!connFrom || !connTo)
    {
        WARN() << "no connector for <" << (!connTo ? xtx->toCurrency : xtx->fromCurrency) << "> " << __FUNCTION__;
        xapp.processLater(txid, packet);
        return true;
    }

    // payTx
    {
        int32_t errCode = 0;
        if (!redeemOrderCounterpartyDeposit(xtx, errCode)) {
            xapp.processLater(txid, packet); // trying again on failure
            return true;
        }
    } // payTx

    xtx->state = TransactionDescr::trCommited;
    xuiConnector.NotifyXBridgeTransactionChanged(txid);

    // send reply
    XBridgePacketPtr reply(new XBridgePacket(xbcTransactionConfirmedB));
    reply->append(hubAddress);
    reply->append(txid.begin(), 32);
    reply->append(xtx->payTxId);

    reply->sign(xtx->mPubKey, xtx->mPrivKey);

    sendPacket(hubAddress, reply);

    return true;
}

//*****************************************************************************
//*****************************************************************************
bool Session::Impl::processTransactionConfirmedB(XBridgePacketPtr packet) const
{
    DEBUG_TRACE();

    // size must be > 52 bytes
    if (packet->size() <= 52 || packet->size() > 1000)
    {
        ERR() << "invalid packet size for xbcTransactionConfirmedB "
              << "need more than 52 bytes and less than 1kb, received " << packet->size() << " "
              << __FUNCTION__;
        return false;
    }

    // check is for me
    if (!checkPacketAddress(packet))
    {
        return true;
    }

    Exchange & e = Exchange::instance();
    if (!e.isStarted())
    {
        return true;
    }

    size_t offset = XBridgePacket::addressSize; // hub address

    std::vector<unsigned char> stxid(packet->data()+offset, packet->data()+offset+XBridgePacket::hashSize);
    uint256 txid(stxid);
    offset += XBridgePacket::hashSize;

    // Pay tx id from B
    std::string b_payTxId(reinterpret_cast<const char *>(packet->data()+offset));

    TransactionPtr tr = e.transaction(txid);
    if (!tr->matches(txid)) // ignore no matching orders
        return true;

    std::vector<unsigned char> pk1(packet->pubkey(), packet->pubkey()+XBridgePacket::pubkeySize);
    if (!packet->verify(tr->b_pk1()))
    {
        WARN() << "bad counterparty packet signature, received " << HexStr(pk1)
               << " expected " << HexStr(tr->b_pk1()) << " " << __FUNCTION__;
        return true;
    }

    if (tr->state() != xbridge::Transaction::trCreated)
    {
        xassert(!"wrong state");
        WARN() << "wrong tx state " << tr->id().ToString()
               << " state " << tr->state()
               << " in " << __FUNCTION__;
        return true;
    }

    tr->updateTimestamp();
    tr->b_setPayTxId(b_payTxId);

    if (e.updateTransactionWhenConfirmedReceived(tr, tr->b_destination()))
    {
        if (tr->state() == xbridge::Transaction::trFinished)
        {
            // Trade completed, no longer need to watch
            xbridge::App & xapp = xbridge::App::instance();
            xapp.unwatchTraderDeposit(tr);

            XBridgePacketPtr reply(new XBridgePacket(xbcTransactionFinished));
            reply->append(txid.begin(), 32);

            reply->sign(e.pubKey(), e.privKey());

            sendPacketBroadcast(reply);
        }
    }

    LOG() << __FUNCTION__ << tr;

    return true;
}

//*****************************************************************************
//*****************************************************************************
bool Session::Impl::processTransactionCancel(XBridgePacketPtr packet) const
{
    DEBUG_TRACE();

    // size must be == 36 bytes
    if (packet->size() != 36)
    {
        ERR() << "invalid packet size for xbcTransactionCancel "
              << "need 36 received " << packet->size() << " "
              << __FUNCTION__;
        return false;
    }

    std::vector<unsigned char> stxid(packet->data(), packet->data()+XBridgePacket::hashSize);
    uint256 txid(stxid);
    TxCancelReason reason = static_cast<TxCancelReason>(*reinterpret_cast<uint32_t*>(packet->data() + 32));

    // check packet signature
    Exchange & e = Exchange::instance();
    if (e.isStarted())
    {
        TransactionPtr tr = e.pendingTransaction(txid);

        if(!tr->isValid())
        {
            tr = e.transaction(txid);
        }

        if(!tr->isValid())
        {
            return true;
        }

        if (!packet->verify(tr->a_pk1()) && !packet->verify(tr->b_pk1()))
        {
            WARN() << "invalid packet signature " << __FUNCTION__;
            return true;
        }

        sendCancelTransaction(tr, reason);
        return true;
    }

    xbridge::App & xapp = xbridge::App::instance();
    TransactionDescrPtr xtx = xapp.transaction(txid);
    if (!xtx)
    {
        return true;
    }

    // Only Maker, Taker, or Servicenode can cancel order
    if (!packet->verify(xtx->sPubKey) && !packet->verify(xtx->oPubKey) && !packet->verify(xtx->mPubKey))
    {
        LOG() << "bad packet signature for cancelation request on order " << xtx->id.GetHex() << " , not canceling " << __FUNCTION__;
        return true;
    }

    WalletConnectorPtr conn = xapp.connectorByCurrency(xtx->fromCurrency);
    if (!conn)
    {
        WARN() << "no connector for <" << xtx->fromCurrency << "> " << __FUNCTION__;
        return false;
    }
    
    // Set order cancel state
    auto cancel = [&xapp, &conn, &xtx, &reason, &txid]() {
        xapp.removePackets(txid);
        xapp.unlockCoins(conn->currency, xtx->usedCoins);
        if (xtx->state < TransactionDescr::trInitialized)
            xapp.unlockFeeUtxos(xtx->feeUtxos);
        xtx->state  = TransactionDescr::trCancelled;
        xtx->reason = reason;
        LOG() << __FUNCTION__ << xtx;
    };

    if (xtx->state < TransactionDescr::trCreated) { // if no deposits yet
        xapp.moveTransactionToHistory(txid);
        cancel();
        xuiConnector.NotifyXBridgeTransactionChanged(txid);
        return true;
    } else if (xtx->state == TransactionDescr::trCancelled) { // already canceled 
        return true;
    } else if (!xtx->didSendDeposit()) { // cancel if deposit not sent
        cancel();
        return true;
    } else if (xtx->hasRedeemedCounterpartyDeposit()) { // Ignore if counterparty deposit already redeemed
        return true;
    }

    // If refund transaction id not defined, do not attempt to rollback
    if (xtx->refTx.empty()) {
        LOG() << "could not find a refund transaction for order " << xtx->id.GetHex() << " " << __FUNCTION__;
        cancel();
        return true;
    }

    // remove from pending packets (if added)
    xapp.removePackets(txid);

    // Set rollback state
    xtx->state = TransactionDescr::trRollback;
    xtx->reason = reason;

    // Attempt to rollback transaction and redeem deposit (this can take time since locktime needs to expire)
    int32_t errCode = 0;
    if (!redeemOrderDeposit(xtx, errCode)) {
        xapp.processLater(txid, packet);
    } else {
        // unlock coins (not fees)
        xapp.unlockCoins(conn->currency, xtx->usedCoins);
    }

    LOG() << __FUNCTION__ << xtx;

    // update transaction state for gui
    xuiConnector.NotifyXBridgeTransactionChanged(txid);

    return true;
}

//*****************************************************************************
//*****************************************************************************
bool Session::Impl::finishTransaction(TransactionPtr tr) const
{
    if (tr == nullptr)
    {
        return false;
    }
    LOG() << "order finished: " << tr->id().GetHex();

    Exchange & e = Exchange::instance();
    if (!e.isStarted())
    {
        return false;
    }

    {
        XBridgePacketPtr reply(new XBridgePacket(xbcTransactionFinished));
        reply->append(tr->id().begin(), 32);

        reply->sign(e.pubKey(), e.privKey());

        sendPacketBroadcast(reply);
    }

    tr->finish();
    return true;
}

//*****************************************************************************
//*****************************************************************************
bool Session::sendCancelTransaction(const TransactionPtr & tx,
                                    const TxCancelReason & reason) const {
    return m_p->sendCancelTransaction(tx, reason);
}

//*****************************************************************************
//*****************************************************************************
bool Session::Impl::sendCancelTransaction(const TransactionPtr & tx,
                                          const TxCancelReason & reason) const
{
    Exchange & e = Exchange::instance();
    if (!e.isStarted())
    {
        return false;
    }

    LOG() << "canceling order " << tx->id().GetHex();

    tx->cancel();
    e.deletePendingTransaction(tx->id());

    XBridgePacketPtr reply(new XBridgePacket(xbcTransactionCancel));
    reply->append(tx->id().begin(), 32);
    reply->append(static_cast<uint32_t>(reason));

    reply->sign(e.pubKey(), e.privKey());

    sendPacketBroadcast(reply);
    return true;
}

//*****************************************************************************
//*****************************************************************************
bool Session::sendCancelTransaction(const TransactionDescrPtr & tx,
                                    const TxCancelReason & reason) const {
    return m_p->sendCancelTransaction(tx, reason);
}

//*****************************************************************************
//*****************************************************************************
bool Session::Impl::sendCancelTransaction(const TransactionDescrPtr & tx,
                                          const TxCancelReason & reason) const
{
    LOG() << "canceling order " << tx->id.GetHex();

    XBridgePacketPtr reply(new XBridgePacket(xbcTransactionCancel));
    reply->append(tx->id.begin(), 32);
    reply->append(static_cast<uint32_t>(reason));

    reply->sign(tx->mPubKey, tx->mPrivKey);

    processTransactionCancel(reply); // process local cancel immediately
    sendPacketBroadcast(reply);

    // update transaction state for gui
    xuiConnector.NotifyXBridgeTransactionChanged(tx->id);

    return true;
}

//*****************************************************************************
//*****************************************************************************
void Session::sendListOfTransactions() const
{
    xbridge::App & xapp = xbridge::App::instance();

    // send exchange trx
    Exchange & e = Exchange::instance();
    if (!e.isStarted())
    {
        return;
    }

    std::list<TransactionPtr> list = e.pendingTransactions();
    std::list<TransactionPtr>::iterator i = list.begin();
    for (; i != list.end(); ++i)
    {
        TransactionPtr & ptr = *i;

        XBridgePacketPtr packet(new XBridgePacket(xbcPendingTransaction));

        // field length must be 8 bytes
        std::vector<unsigned char> fc(8, 0);
        std::string tmp = ptr->a_currency();
        std::copy(tmp.begin(), tmp.end(), fc.begin());

        // field length must be 8 bytes
        std::vector<unsigned char> tc(8, 0);
        tmp = ptr->b_currency();
        std::copy(tmp.begin(), tmp.end(), tc.begin());

        packet->append(ptr->id().begin(), 32);
        packet->append(fc);
        packet->append(ptr->a_amount());
        packet->append(tc);
        packet->append(ptr->b_amount());
        packet->append(m_p->m_myid);
        packet->append(xbridge::timeToInt(ptr->createdTime()));
        packet->append(ptr->blockHash().begin(), 32);

        packet->sign(e.pubKey(), e.privKey());

        m_p->sendPacketBroadcast(packet);
    }
}

void Session::Impl::sendTransaction(uint256 & id) const
{
    Exchange & e = Exchange::instance();
    if (!e.isStarted())
    {
        return;
    }

    TransactionPtr tr = e.pendingTransaction(id);
    if (!tr->matches(id))
        return;

    XBridgePacketPtr packet(new XBridgePacket(xbcPendingTransaction));

    // field length must be 8 bytes
    std::vector<unsigned char> fc(8, 0);
    std::string tmp = tr->a_currency();
    std::copy(tmp.begin(), tmp.end(), fc.begin());

    // field length must be 8 bytes
    std::vector<unsigned char> tc(8, 0);
    tmp = tr->b_currency();
    std::copy(tmp.begin(), tmp.end(), tc.begin());

    packet->append(tr->id().begin(), 32);
    packet->append(fc);
    packet->append(tr->a_amount());
    packet->append(tc);
    packet->append(tr->b_amount());
    packet->append(m_myid);
    packet->append(xbridge::timeToInt(tr->createdTime()));
    packet->append(tr->blockHash().begin(), 32);

    packet->sign(e.pubKey(), e.privKey());

    sendPacketBroadcast(packet);
}

//*****************************************************************************
//*****************************************************************************
void Session::checkFinishedTransactions() const
{
    Exchange & e = Exchange::instance();
    if (!e.isStarted())
    {
        return;
    }

    std::list<TransactionPtr> list = e.finishedTransactions();
    std::list<TransactionPtr>::iterator i = list.begin();
    for (; i != list.end(); ++i)
    {
        TransactionPtr & ptr = *i;

        uint256 txid = ptr->id();

        if (ptr->state() == xbridge::Transaction::trCancelled)
        {
            // drop cancelled tx
            LOG() << "drop cancelled transaction <" << txid.GetHex() << ">";
            ptr->drop();
        }
        else if (ptr->state() == xbridge::Transaction::trFinished)
        {
            // delete finished tx
            LOG() << "delete finished transaction <" << txid.GetHex() << ">";
            e.deleteTransaction(txid);
        }
        else if (ptr->state() == xbridge::Transaction::trDropped)
        {
            // delete dropped tx
            LOG() << "delete dropped transaction <" << txid.GetHex() << ">";
            e.deleteTransaction(txid);
        }
        else if (!ptr->isValid())
        {
            // delete invalid tx
            LOG() << "delete invalid transaction <" << txid.GetHex() << ">";
            e.deleteTransaction(txid);
        }
        else
        {
            LOG() << "timeout transaction <" << txid.GetHex() << ">"
                  << " state " << ptr->strState();

            // send rollback
            m_p->sendCancelTransaction(ptr, TxCancelReason::crTimeout);
        }
    }
}

//*****************************************************************************
//*****************************************************************************
void Session::getAddressBook() const
{
    App & xapp = App::instance();
    Connectors conns = xapp.connectors();

    for (Connectors::iterator i = conns.begin(); i != conns.end(); ++i)
    {
        std::string currency = (*i)->currency;

        std::vector<wallet::AddressBookEntry> entries;
        (*i)->requestAddressBook(entries);

        for (const wallet::AddressBookEntry & e : entries)
        {
            for (const std::string & addr : e.second)
            {
                std::vector<unsigned char> vaddr = (*i)->toXAddr(addr);

                xapp.updateConnector(*i, vaddr, currency);

                xuiConnector.NotifyXBridgeAddressBookEntryReceived
                        ((*i)->currency, e.first, addr);
            }
        }
    }
}

//******************************************************************************
//******************************************************************************
bool Session::Impl::processTransactionFinished(XBridgePacketPtr packet) const
{
    DEBUG_TRACE();


    if (packet->size() != 32)
    {
        ERR() << "invalid packet size for xbcTransactionFinished "
              << "need 32 bytes, received " << packet->size() << " "
              << __FUNCTION__;
        return false;
    }

    // transaction id
    std::vector<unsigned char> stxid(packet->data(), packet->data()+XBridgePacket::hashSize);
    uint256 txid(stxid);

    xbridge::App & xapp = xbridge::App::instance();

    TransactionDescrPtr xtx = xapp.transaction(txid);
    if (xtx == nullptr)
    {
        LOG() << "unknown order " << HexStr(txid) << " " << __FUNCTION__;
        return true;
    }
    std::vector<unsigned char> spubkey(packet->pubkey(), packet->pubkey()+XBridgePacket::pubkeySize);
    if (!packet->verify(xtx->sPubKey))
    {
        WARN() << "wrong servicenode handling order, expected " << HexStr(xtx->sPubKey)
               << " but received pubkey " << HexStr(spubkey) << " " << __FUNCTION__;
        return true;
    }

    // update transaction state for gui
    xtx->state = TransactionDescr::trFinished;

    LOG() << __FUNCTION__ << xtx;

    xapp.moveTransactionToHistory(txid);
    xuiConnector.NotifyXBridgeTransactionChanged(txid);

    return true;
}

bool Session::redeemOrderDeposit(const TransactionDescrPtr & xtx, int32_t & errCode) const {
    return m_p->redeemOrderDeposit(xtx, errCode);
}

bool Session::redeemOrderCounterpartyDeposit(const TransactionDescrPtr & xtx, int32_t & errCode) const {
    return m_p->redeemOrderCounterpartyDeposit(xtx, errCode);
}

bool Session::refundTraderDeposit(const std::string & orderId, const std::string & currency, const uint32_t & lockTime,
                         const std::string & refTx, int32_t & errCode) const {
    return m_p->refundTraderDeposit(orderId, currency, lockTime, refTx, errCode);
}

bool Session::Impl::redeemOrderDeposit(const TransactionDescrPtr & xtx, int32_t & errCode) const {
    xbridge::App & xapp = xbridge::App::instance();
    WalletConnectorPtr connFrom = xapp.connectorByCurrency(xtx->fromCurrency);
    if (!connFrom) {
        WARN() << "rollback attempted failed, no connector for <" << xtx->fromCurrency << "> is the wallet running?";
        return false;
    }

    auto & txid = xtx->id;
    if (xtx->state < TransactionDescr::trCreated) {
        return true; // done
    }

    // If refund transaction id not defined, do not attempt to rollback
    if (xtx->refTx.empty()) {
        WalletConnectorPtr connTo = xapp.connectorByCurrency(xtx->toCurrency);
        auto fromAddr = connFrom->fromXAddr(xtx->from);
        auto toAddr = connTo ? connTo->fromXAddr(xtx->to) : "";
        if (!xtx->binTx.empty()) // if there's a bin tx but no rollback, could mean potential loss of funds
            LOG() << "Fatal error, unable to rollback. Could not find a refund transaction for order " << xtx->id.GetHex() << " "
                  << xtx->fromCurrency << "(" << xbridge::xBridgeStringValueFromAmount(xtx->fromAmount) << " - " << fromAddr << ") / "
                  << xtx->toCurrency   << "(" << xbridge::xBridgeStringValueFromAmount(xtx->toAmount)   << " - " << toAddr   << ")";
        return true; // done
    }

    xbridge::rpc::WalletInfo info;
    bool infoReceived = connFrom->getInfo(info);

    // Check if locktime for the deposit has expired (can't redeem until locktime expires)
    if (infoReceived && info.blocks < xtx->lockTime)
    {
        LOG() << "will be able to redeem canceled order " << txid.GetHex() << " (" << xtx->fromCurrency << ") when locktime expires "
              << "at block " << xtx->lockTime << " , deposit txid is " << xtx->binTxId << " "
              << __FUNCTION__;
        return false;
    }
    else // if locktime has expired, attempt to send refund tx
    {
        std::string sid;
        int32_t errCode = 0;
        std::string errorMessage;
        if (!connFrom->sendRawTransaction(xtx->refTx, sid, errCode, errorMessage))
        {
            WalletConnectorPtr connTo = xapp.connectorByCurrency(xtx->toCurrency);
            auto fromAddr = connFrom->fromXAddr(xtx->from);
            auto toAddr = connTo ? connTo->fromXAddr(xtx->to) : "";
            LOG() << "failed to rollback locked deposit funds for order " << txid.GetHex() << " "
                    << xtx->fromCurrency << "(" << xbridge::xBridgeStringValueFromAmount(xtx->fromAmount) << " - " << fromAddr << ") / "
                    << xtx->toCurrency   << "(" << xbridge::xBridgeStringValueFromAmount(xtx->toAmount)   << " - " << toAddr   << ")"
                    << " trying again later";
            xtx->state = TransactionDescr::trRollbackFailed;
            return false;
        } else {
            xtx->state = TransactionDescr::trRollback;
        }
    }

    xuiConnector.NotifyXBridgeTransactionChanged(txid);

    return true; // success
}

bool Session::Impl::redeemOrderCounterpartyDeposit(const TransactionDescrPtr & xtx, int32_t & errCode) const {
    xbridge::App & xapp = xbridge::App::instance();
    WalletConnectorPtr connFrom = xapp.connectorByCurrency(xtx->fromCurrency);
    WalletConnectorPtr connTo = xapp.connectorByCurrency(xtx->toCurrency);
    if (!connFrom || !connTo) {
        ERR() << "failed to redeem order due to bad wallet connection, is "
              << (!connFrom ? xtx->fromCurrency : xtx->toCurrency) << " running?";
        return false;
    }

    // Try and find the secret
    if (!xtx->hasSecret()) {
        std::vector<unsigned char> x;
        bool isGood = false;
        if (!connFrom->getSecretFromPaymentTransaction(xtx->otherPayTxId(), xtx->binTxId, xtx->binTxVout, xtx->oHashedSecret, x, isGood))
        {
            // Keep looking for the maker pay tx, move packet to pending
            return false;
        }
        else if (!isGood)
        {
            ERR() << "secret not found in counterparty's pay tx on " << xtx->fromCurrency << " " << xtx->otherPayTxId()
                  << " my deposit utxo they spent is " << xtx->binTxId << " vout " << xtx->binTxVout
                  << " counterparty could be misbehaving";
            return false;
        }

        // assign the secret
        xtx->setSecret(x);
        // done watching for spent pay tx
        xtx->doneWatching();
        xapp.unwatchSpentDeposit(xtx);
    }

    auto fromAddr = connFrom->fromXAddr(xtx->from);
    auto toAddr = connTo->fromXAddr(xtx->to);

    double outAmount   = static_cast<double>(xtx->toAmount)/TransactionDescr::COIN;
    double checkAmount = outAmount;
    std::vector<xbridge::XTxIn>                  inputs;
    std::vector<std::pair<std::string, double> > outputs;

    // inputs from binTx
    inputs.emplace_back(xtx->oBinTxId, xtx->oBinTxVout, checkAmount);

    // outputs
    {
        outputs.emplace_back(toAddr, outAmount + xtx->oOverpayment);
    }

    if (!connTo->createPaymentTransaction(inputs, outputs,
                                          xtx->mPubKey, xtx->mPrivKey,
                                          xtx->secret(), xtx->unlockScript,
                                          xtx->payTxId, xtx->payTx))
    {
        ERR() << "failed to create payment redeem transaction, retrying " << __FUNCTION__;
        TXLOG() << "redeem counterparty deposit for order " << xtx->id.ToString() << " (submit manually using sendrawtransaction) "
                << xtx->fromCurrency << "(" << xbridge::xBridgeStringValueFromAmount(xtx->fromAmount) << " - " << fromAddr << ") / "
                << xtx->toCurrency   << "(" << xbridge::xBridgeStringValueFromAmount(xtx->toAmount)   << " - " << toAddr   << ")" << std::endl
                << xtx->payTx;
        return false;
    }

    TXLOG() << "redeem counterparty deposit for order " << xtx->id.ToString() << " (submit manually using sendrawtransaction) "
            << xtx->fromCurrency << "(" << xbridge::xBridgeStringValueFromAmount(xtx->fromAmount) << " - " << fromAddr << ") / "
            << xtx->toCurrency   << "(" << xbridge::xBridgeStringValueFromAmount(xtx->toAmount)   << " - " << toAddr   << ")" << std::endl
            << xtx->payTx;

    // send pay tx
    std::string sentid;
    std::string errorMessage;
    if (connTo->sendRawTransaction(xtx->payTx, sentid, errCode, errorMessage))
    {
        LOG() << "redeeming order " << xtx->id.ToString() << " from counterparty on " << xtx->toCurrency << " chain with pay txid " << xtx->payTxId;
    }
    else
    {
        if (errCode == RPCErrorCode::RPC_VERIFY_ALREADY_IN_CHAIN) {
            LOG() << "redeem tx already found in chain, proceeding";
        }
        else
        {
            if (errCode == RPCErrorCode::RPC_VERIFY_ERROR) { // missing inputs, wait deposit tx
                LOG() << "failed to redeem tx from counterparty: bad inputs";
            } else {
                LOG() << "failed to redeem tx from counterparty";
            }
            return false; // can't cancel, maker already redeemed taker funds
        }
    }

    // Note that we've been paid
    xtx->counterpartyDepositRedeemed();

    return true;
}

bool Session::Impl::refundTraderDeposit(const std::string & orderId, const std::string & currency, const uint32_t & lockTime,
                                        const std::string & refTx, int32_t & errCode) const
{
    xbridge::App & xapp = xbridge::App::instance();
    WalletConnectorPtr conn = xapp.connectorByCurrency(currency);
    if (!conn) {
        WARN() << "refund attempt failed, no connector for trader (" << currency << ") on order "
               << orderId << " , is the wallet running?";
        return false;
    }

    // If refund transaction id not defined, do not attempt to rollback
    if (refTx.empty()) {
        LOG() << "Fatal error, unable to submit refund for trader (" << currency << ") on order "
              << orderId << " due to an unknown refund tx";
        errCode = RPCErrorCode::RPC_MISC_ERROR;
        return true; // done
    }

    std::string sid;
    std::string errorMessage;
    if (!conn->sendRawTransaction(refTx, sid, errCode, errorMessage))
        return false;

    return true; // success
}

} // namespace xbridge
