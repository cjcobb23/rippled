//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2017 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/misc/TxQ.h>
#include <ripple/basics/mulDiv.h>
#include <ripple/core/DatabaseCon.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/protocol/jss.h>
#include <ripple/rpc/impl/RPCHelpers.h>
#include <test/jtx.h>
#include <test/jtx/Env.h>
#include <test/jtx/envconfig.h>

#include <ripple/rpc/GRPCHandlers.h>
#include <ripple/rpc/impl/RPCHelpers.h>
#include <test/rpc/GRPCTestClientBase.h>

namespace ripple {
namespace test {

class Tx_test : public beast::unit_test::suite
{
    template <class T>
    std::string
    toByteString(T const& data)
    {
        const char* bytes = reinterpret_cast<const char*>(data.data());
        return {bytes, data.size()};
    }

    void
    cmpAmount(const rpc::v1::CurrencyAmount& proto_amount, STAmount amount)
    {
        if (amount.native())
        {
            BEAST_EXPECT(
                proto_amount.xrp_amount().drops() == amount.xrp().drops());
        }
        else
        {
            rpc::v1::IssuedCurrencyAmount issuedCurrency =
                proto_amount.issued_currency_amount();
            Issue const& issue = amount.issue();
            Currency currency = issue.currency;
            BEAST_EXPECT(
                issuedCurrency.currency().name() == to_string(currency));
            BEAST_EXPECT(
                issuedCurrency.currency().code() == toByteString(currency));
            BEAST_EXPECT(issuedCurrency.value() == to_string(amount.iou()));
            BEAST_EXPECT(
                issuedCurrency.issuer().address() == toBase58(issue.account));
        }
    }

    void
    cmpTx(const rpc::v1::Transaction& proto, std::shared_ptr<STTx const> txn_st)
    {
        AccountID account = txn_st->getAccountID(sfAccount);
        BEAST_EXPECT(proto.account().address() == toBase58(account));

        STAmount amount = txn_st->getFieldAmount(sfAmount);
        cmpAmount(proto.payment().amount(), amount);

        AccountID account_dest = txn_st->getAccountID(sfDestination);
        BEAST_EXPECT(
            proto.payment().destination().address() == toBase58(account_dest));

        STAmount fee = txn_st->getFieldAmount(sfFee);
        BEAST_EXPECT(proto.fee().drops() == fee.xrp().drops());

        BEAST_EXPECT(proto.sequence() == txn_st->getFieldU32(sfSequence));

        Blob signingPubKey = txn_st->getFieldVL(sfSigningPubKey);
        BEAST_EXPECT(proto.signing_public_key() == toByteString(signingPubKey));

        BEAST_EXPECT(proto.flags() == txn_st->getFieldU32(sfFlags));

        BEAST_EXPECT(
            proto.last_ledger_sequence() ==
            txn_st->getFieldU32(sfLastLedgerSequence));

        Blob blob = txn_st->getFieldVL(sfTxnSignature);
        BEAST_EXPECT(proto.signature() == toByteString(blob));

        if (txn_st->isFieldPresent(sfSendMax))
        {
            STAmount const& send_max = txn_st->getFieldAmount(sfSendMax);
            cmpAmount(proto.payment().send_max(), send_max);
        }

        // populate path data
        STPathSet const& pathset = txn_st->getFieldPathSet(sfPaths);
        int ind = 0;
        for (auto it = pathset.begin(); it < pathset.end(); ++it)
        {
            STPath const& path = *it;

            const rpc::v1::Path& proto_path = proto.payment().paths(ind++);

            int ind2 = 0;
            for (auto it2 = path.begin(); it2 != path.end(); ++it2)
            {
                const rpc::v1::PathElement& proto_element =
                    proto_path.elements(ind2++);
                STPathElement const& elt = *it2;

                if (elt.isOffer())
                {
                    if (elt.hasCurrency())
                    {
                        Currency const& currency = elt.getCurrency();
                        BEAST_EXPECT(
                            proto_element.currency().name() ==
                            to_string(currency));
                    }
                    if (elt.hasIssuer())
                    {
                        AccountID const& issuer = elt.getIssuerID();
                        BEAST_EXPECT(
                            proto_element.issuer().address() ==
                            toBase58(issuer));
                    }
                }
                else
                {
                    AccountID const& path_account = elt.getAccountID();
                    BEAST_EXPECT(
                        proto_element.account().address() ==
                        toBase58(path_account));
                }
            }
        }
    }

    void
    cmpMeta(const rpc::v1::Meta& proto, std::shared_ptr<TxMeta> txMeta)
    {
        BEAST_EXPECT(proto.transaction_index() == txMeta->getIndex());
        BEAST_EXPECT(
            proto.transaction_result().result() ==
            transToken(txMeta->getResultTER()));

        rpc::v1::TransactionResult r;

        RPC::populateTransactionResultType(r, txMeta->getResultTER());

        BEAST_EXPECT(
            proto.transaction_result().result_type() == r.result_type());

        if (txMeta->hasDeliveredAmount())
        {
            cmpAmount(proto.delivered_amount(), txMeta->getDeliveredAmount());
        }
    }

    // gRPC stuff
    class GrpcTxClient : public GRPCTestClientBase
    {
    public:
        rpc::v1::GetTxRequest request;
        rpc::v1::GetTxResponse reply;

        GrpcTxClient(std::string const& port) : GRPCTestClientBase(port)
        {
        }

        void
        Tx()
        {
            status = stub_->GetTx(&context, request, &reply);
        }
    };

    void
    testTxGrpc()
    {
        testcase("Test Tx Grpc");

        using namespace test::jtx;
        std::unique_ptr<Config> config = envconfig(addGrpcConfig);
        std::string grpcPort = *(*config)["port_grpc"].get<std::string>("port");
        Env env(*this, std::move(config));

        auto grpcTx = [&grpcPort](auto hash, auto binary) {
            GrpcTxClient client(grpcPort);
            client.request.set_hash(&hash, sizeof(hash));
            client.request.set_binary(binary);
            client.Tx();
            return std::pair<bool, rpc::v1::GetTxResponse>(
                client.status.ok(), client.reply);
        };

        Account A1{"A1"};
        Account A2{"A2"};
        env.fund(XRP(10000), A1);
        env.fund(XRP(10000), A2);
        env.close();
        env.trust(A2["USD"](1000), A1);
        env.close();
        std::vector<std::shared_ptr<STTx const>> txns;
        auto const startLegSeq = env.current()->info().seq;
        for (int i = 0; i < 14; ++i)
        {
            if (i & 1)
                env(pay(A2, A1, A2["USD"](100)));
            else
                env(pay(A2, A1, A2["XRP"](200)));
            txns.emplace_back(env.tx());
            env.close();
        }
        auto const endLegSeq = env.closed()->info().seq;

        // Find the existing transactions
        auto& ledgerMaster = env.app().getLedgerMaster();
        int index = startLegSeq;
        for (auto&& tx : txns)
        {
            auto id = tx->getTransactionID();
            auto ledger = ledgerMaster.getLedgerBySeq(index);

            for (bool b : {false, true})
            {
                auto const result = grpcTx(id, b);

                BEAST_EXPECT(result.first == true);
                BEAST_EXPECT(result.second.ledger_index() == index);
                BEAST_EXPECT(result.second.validated() == true);
                if (b)
                {
                    Serializer s = tx->getSerializer();
                    BEAST_EXPECT(
                        result.second.transaction_binary() == toByteString(s));
                }
                else
                {
                    cmpTx(result.second.transaction(), tx);
                }

                if (ledger && !b)
                {
                    auto rawMeta = ledger->txRead(id).second;
                    if (rawMeta)
                    {
                        auto txMeta = std::make_shared<TxMeta>(
                            id, ledger->seq(), *rawMeta);

                        cmpMeta(result.second.meta(), txMeta);
                    }
                }
            }
            index++;
        }

        // Find not existing transaction
        auto const tx = env.jt(noop(A1), seq(env.seq(A1))).stx;
        for (bool b : {false, true})
        {
            auto const result = grpcTx(tx->getTransactionID(), b);

            BEAST_EXPECT(result.first == false);
        }

        // Delete one transaction
        const auto deletedLedger = (startLegSeq + endLegSeq) / 2;
        {
            // Remove one of the ledgers from the database directly
            auto db = env.app().getTxnDB().checkoutDb();
            *db << "DELETE FROM Transactions WHERE LedgerSeq == "
                << deletedLedger << ";";
        }

        for (bool b : {false, true})
        {
            auto const result = grpcTx(tx->getTransactionID(), b);

            BEAST_EXPECT(result.first == false);
        }
    }

public:
    void
    run() override
    {
        testTxGrpc();
    }
};

BEAST_DEFINE_TESTSUITE(Tx, app, ripple);
}  // namespace test
}  // namespace ripple
