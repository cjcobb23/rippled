//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2016 Ripple Labs Inc.

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
#include <test/jtx.h>
#include <ripple/beast/unit_test.h>
#include <ripple/protocol/jss.h>
#include <ripple/protocol/SField.h>
#include <cstdlib>

#include <ripple/rpc/GRPCHandlers.h>
#include <ripple/rpc/impl/RPCHelpers.h>
#include <test/rpc/GRPCTestClientBase.h>


namespace ripple {

class AccountTxPaging_test : public beast::unit_test::suite
{
    bool
    checkTransaction (Json::Value const& tx, int sequence, int ledger)
    {
        return (
            tx[jss::tx][jss::Sequence].asInt() == sequence &&
            tx[jss::tx][jss::ledger_index].asInt() == ledger);
    }

    auto next(
        test::jtx::Env & env,
        test::jtx::Account const& account,
        int ledger_min,
        int ledger_max,
        int limit,
        bool forward,
        Json::Value const& marker = Json::nullValue)
    {
        Json::Value jvc;
        jvc[jss::account] = account.human();
        jvc[jss::ledger_index_min] = ledger_min;
        jvc[jss::ledger_index_max] = ledger_max;
        jvc[jss::forward] = forward;
        jvc[jss::limit] = limit;
        if (marker)
            jvc[jss::marker] = marker;

        return env.rpc("json", "account_tx", to_string(jvc))[jss::result];
    }

    void
    testAccountTxPaging ()
    {
        testcase("Paging for Single Account");
        using namespace test::jtx;

        Env env(*this);
        Account A1 {"A1"};
        Account A2 {"A2"};
        Account A3 {"A3"};

        env.fund(XRP(10000), A1, A2, A3);
        env.close();

        env.trust(A3["USD"](1000), A1);
        env.trust(A2["USD"](1000), A1);
        env.trust(A3["USD"](1000), A2);
        env.close();

        for (auto i = 0; i < 5; ++i)
        {
            env(pay(A2, A1, A2["USD"](2)));
            env(pay(A3, A1, A3["USD"](2)));
            env(offer(A1, XRP(11), A1["USD"](1)));
            env(offer(A2, XRP(10), A2["USD"](1)));
            env(offer(A3, XRP(9),  A3["USD"](1)));
            env.close();
        }

        /* The sequence/ledger for A3 are as follows:
         * seq     ledger_index
         * 3  ----> 3
         * 1  ----> 3
         * 2  ----> 4
         * 2  ----> 4
         * 2  ----> 5
         * 3  ----> 5
         * 4  ----> 6
         * 5  ----> 6
         * 6  ----> 7
         * 7  ----> 7
         * 8  ----> 8
         * 9  ----> 8
         * 10 ----> 9
         * 11 ----> 9
        */

        // page through the results in several ways.
        {
            // limit = 2, 3 batches giving the first 6 txs
            auto jrr = next(env, A3, 2, 5, 2, true);
            auto txs = jrr[jss::transactions];
            if (! BEAST_EXPECT(txs.isArray() && txs.size() == 2))
                return;
            BEAST_EXPECT(checkTransaction (txs[0u], 3, 3));
            BEAST_EXPECT(checkTransaction (txs[1u], 3, 3));
            if(! BEAST_EXPECT(jrr[jss::marker]))
                return;

            jrr = next(env, A3, 2, 5, 2, true, jrr[jss::marker]);
            txs = jrr[jss::transactions];
            if (! BEAST_EXPECT(txs.isArray() && txs.size() == 2))
                return;
            BEAST_EXPECT(checkTransaction (txs[0u], 4, 4));
            BEAST_EXPECT(checkTransaction (txs[1u], 4, 4));
            if(! BEAST_EXPECT(jrr[jss::marker]))
                return;

            jrr = next(env, A3, 2, 5, 2, true, jrr[jss::marker]);
            txs = jrr[jss::transactions];
            if (! BEAST_EXPECT(txs.isArray() && txs.size() == 2))
                return;
            BEAST_EXPECT(checkTransaction (txs[0u], 4, 5));
            BEAST_EXPECT(checkTransaction (txs[1u], 5, 5));
            BEAST_EXPECT(! jrr[jss::marker]);
            return;
        }

        {
            // limit 1, 3 requests giving the first 3 txs
            auto jrr = next(env, A3, 3, 9, 1, true);
            auto txs = jrr[jss::transactions];
            if (! BEAST_EXPECT(txs.isArray() && txs.size() == 1))
                return;
            BEAST_EXPECT(checkTransaction (txs[0u], 3, 3));
            if(! BEAST_EXPECT(jrr[jss::marker]))
                return;

            jrr = next(env, A3, 3, 9, 1, true, jrr[jss::marker]);
            txs = jrr[jss::transactions];
            if (! BEAST_EXPECT(txs.isArray() && txs.size() == 1))
                return;
            BEAST_EXPECT(checkTransaction (txs[0u], 3, 3));
            if(! BEAST_EXPECT(jrr[jss::marker]))
                return;

            jrr = next(env, A3, 3, 9, 1, true, jrr[jss::marker]);
            txs = jrr[jss::transactions];
            if (! BEAST_EXPECT(txs.isArray() && txs.size() == 1))
                return;
            BEAST_EXPECT(checkTransaction (txs[0u], 4, 4));
            if(! BEAST_EXPECT(jrr[jss::marker]))
                return;

            // continue with limit 3, to end of all txs
            jrr = next(env, A3, 3, 9, 3, true, jrr[jss::marker]);
            txs = jrr[jss::transactions];
            if (! BEAST_EXPECT(txs.isArray() && txs.size() == 3))
                return;
            BEAST_EXPECT(checkTransaction (txs[0u], 4, 4));
            BEAST_EXPECT(checkTransaction (txs[1u], 4, 5));
            BEAST_EXPECT(checkTransaction (txs[2u], 5, 5));
            if(! BEAST_EXPECT(jrr[jss::marker]))
                return;

            jrr = next(env, A3, 3, 9, 3, true, jrr[jss::marker]);
            txs = jrr[jss::transactions];
            if (! BEAST_EXPECT(txs.isArray() && txs.size() == 3))
                return;
            BEAST_EXPECT(checkTransaction (txs[0u], 6, 6));
            BEAST_EXPECT(checkTransaction (txs[1u], 7, 6));
            BEAST_EXPECT(checkTransaction (txs[2u], 8, 7));
            if(! BEAST_EXPECT(jrr[jss::marker]))
                return;

            jrr = next(env, A3, 3, 9, 3, true, jrr[jss::marker]);
            txs = jrr[jss::transactions];
            if (! BEAST_EXPECT(txs.isArray() && txs.size() == 3))
                return;
            BEAST_EXPECT(checkTransaction (txs[0u], 9, 7));
            BEAST_EXPECT(checkTransaction (txs[1u], 10, 8));
            BEAST_EXPECT(checkTransaction (txs[2u], 11, 8));
            if(! BEAST_EXPECT(jrr[jss::marker]))
                return;

            jrr = next(env, A3, 3, 9, 3, true, jrr[jss::marker]);
            txs = jrr[jss::transactions];
            if (! BEAST_EXPECT(txs.isArray() && txs.size() == 2))
                return;
            BEAST_EXPECT(checkTransaction (txs[0u], 12, 9));
            BEAST_EXPECT(checkTransaction (txs[1u], 13, 9));
            BEAST_EXPECT(! jrr[jss::marker]);
        }

        {
            // limit 2, descending, 2 batches giving last 4 txs
            auto jrr = next(env, A3, 3, 9, 2, false);
            auto txs = jrr[jss::transactions];
            if (! BEAST_EXPECT(txs.isArray() && txs.size() == 2))
                return;
            BEAST_EXPECT(checkTransaction (txs[0u], 13, 9));
            BEAST_EXPECT(checkTransaction (txs[1u], 12, 9));
            if(! BEAST_EXPECT(jrr[jss::marker]))
                return;

            jrr = next(env, A3, 3, 9, 2, false, jrr[jss::marker]);
            txs = jrr[jss::transactions];
            if (! BEAST_EXPECT(txs.isArray() && txs.size() == 2))
                return;
            BEAST_EXPECT(checkTransaction (txs[0u], 11, 8));
            BEAST_EXPECT(checkTransaction (txs[1u], 10, 8));
            if(! BEAST_EXPECT(jrr[jss::marker]))
                return;

            // continue with limit 3 until all txs have been seen
            jrr = next(env, A3, 3, 9, 3, false, jrr[jss::marker]);
            txs = jrr[jss::transactions];
            if (! BEAST_EXPECT(txs.isArray() && txs.size() == 3))
                return;
            BEAST_EXPECT(checkTransaction (txs[0u], 9, 7));
            BEAST_EXPECT(checkTransaction (txs[1u], 8, 7));
            BEAST_EXPECT(checkTransaction (txs[2u], 7, 6));
            if(! BEAST_EXPECT(jrr[jss::marker]))
                return;

            jrr = next(env, A3, 3, 9, 3, false, jrr[jss::marker]);
            txs = jrr[jss::transactions];
            if (! BEAST_EXPECT(txs.isArray() && txs.size() == 3))
                return;
            BEAST_EXPECT(checkTransaction (txs[0u], 6, 6));
            BEAST_EXPECT(checkTransaction (txs[1u], 5, 5));
            BEAST_EXPECT(checkTransaction (txs[2u], 4, 5));
            if(! BEAST_EXPECT(jrr[jss::marker]))
                return;

            jrr = next(env, A3, 3, 9, 3, false, jrr[jss::marker]);
            txs = jrr[jss::transactions];
            if (! BEAST_EXPECT(txs.isArray() && txs.size() == 3))
                return;
            BEAST_EXPECT(checkTransaction (txs[0u], 4, 4));
            BEAST_EXPECT(checkTransaction (txs[1u], 4, 4));
            BEAST_EXPECT(checkTransaction (txs[2u], 3, 3));
            if(! BEAST_EXPECT(jrr[jss::marker]))
                return;

            jrr = next(env, A3, 3, 9, 3, false, jrr[jss::marker]);
            txs = jrr[jss::transactions];
            if (! BEAST_EXPECT(txs.isArray() && txs.size() == 1))
                return;
            BEAST_EXPECT(checkTransaction (txs[0u], 3, 3));
            BEAST_EXPECT(! jrr[jss::marker]);
        }
    }

    class GrpcAccountTxClient : public test::GRPCTestClientBase
    {
    public:
        org::xrpl::rpc::v1::GetAccountTransactionHistoryRequest request;
        org::xrpl::rpc::v1::GetAccountTransactionHistoryResponse reply;

        explicit GrpcAccountTxClient(std::string const& port)
            : GRPCTestClientBase(port)
        {
        }

        void
        AccountTx()
        {
            status =
                stub_->GetAccountTransactionHistory(&context, request, &reply);
        }
    };

    bool
    checkTransaction(
        org::xrpl::rpc::v1::GetTransactionResponse const& tx,
        int sequence,
        int ledger)
    {
        return (
            tx.transaction().sequence().value() == sequence &&
            tx.ledger_index() == ledger);
    }

    std::pair<
        org::xrpl::rpc::v1::GetAccountTransactionHistoryResponse,
        grpc::Status>
    nextBinary(
        std::string grpcPort,
        test::jtx::Env& env,
        std::string const& account = "",
        int ledger_min = -1,
        int ledger_max = -1,
        int limit = -1,
        bool forward = false,
        org::xrpl::rpc::v1::Marker* marker = nullptr)
    {
        GrpcAccountTxClient client{grpcPort};
        auto& request = client.request;
        if (account != "")
            request.mutable_account()->set_address(account);
        if (ledger_min != -1)
            request.mutable_ledger_range()->set_ledger_index_min(ledger_min);
        if (ledger_max != -1)
            request.mutable_ledger_range()->set_ledger_index_max(ledger_max);
        request.set_forward(forward);
        request.set_binary(true);
        if (limit != -1)
            request.set_limit(limit);
        if (marker)
        {
            *request.mutable_marker() = *marker;
        }

        client.AccountTx();
        return {client.reply, client.status};
    }

    std::pair<
        org::xrpl::rpc::v1::GetAccountTransactionHistoryResponse,
        grpc::Status>
    next(
        std::string grpcPort,
        test::jtx::Env& env,
        std::string const& account = "",
        int ledger_min = -1,
        int ledger_max = -1,
        int limit = -1,
        bool forward = false,
        org::xrpl::rpc::v1::Marker* marker = nullptr)
    {
        GrpcAccountTxClient client{grpcPort};
        auto& request = client.request;
        if (account != "")
            request.mutable_account()->set_address(account);
        if (ledger_min != -1)
            request.mutable_ledger_range()->set_ledger_index_min(ledger_min);
        if (ledger_max != -1)
            request.mutable_ledger_range()->set_ledger_index_max(ledger_max);
        request.set_forward(forward);
        if (limit != -1)
            request.set_limit(limit);
        if (marker)
        {
            *request.mutable_marker() = *marker;
        }

        client.AccountTx();
        return {client.reply, client.status};
    }

    std::pair<
        org::xrpl::rpc::v1::GetAccountTransactionHistoryResponse,
        grpc::Status>
    nextWithSeq(
        std::string grpcPort,
        test::jtx::Env& env,
        std::string const& account = "",
        int ledger_seq = -1,
        int limit = -1,
        bool forward = false,
        org::xrpl::rpc::v1::Marker* marker = nullptr)
    {
        GrpcAccountTxClient client{grpcPort};
        auto& request = client.request;
        if (account != "")
            request.mutable_account()->set_address(account);
        if (ledger_seq != -1)
            request.mutable_ledger_specifier()->set_sequence(ledger_seq);
        request.set_forward(forward);
        if (limit != -1)
            request.set_limit(limit);
        if (marker)
        {
            *request.mutable_marker() = *marker;
        }

        client.AccountTx();
        return {client.reply, client.status};
    }

    std::pair<
        org::xrpl::rpc::v1::GetAccountTransactionHistoryResponse,
        grpc::Status>
    nextWithHash(
        std::string grpcPort,
        test::jtx::Env& env,
        std::string const& account = "",
        uint256 const& hash = beast::zero,
        int limit = -1,
        bool forward = false,
        org::xrpl::rpc::v1::Marker* marker = nullptr)
    {
        GrpcAccountTxClient client{grpcPort};
        auto& request = client.request;
        if (account != "")
            request.mutable_account()->set_address(account);
        if (hash != beast::zero)
            request.mutable_ledger_specifier()->set_hash(
                hash.data(), hash.size());
        request.set_forward(forward);
        if (limit != -1)
            request.set_limit(limit);
        if (marker)
        {
            *request.mutable_marker() = *marker;
        }

        client.AccountTx();
        return {client.reply, client.status};
    }

    void
    testAccountTxParametersGrpc()
    {
        testcase("Test Account_tx Grpc");

        using namespace test::jtx;
        std::unique_ptr<Config> config = envconfig(addGrpcConfig);
        std::string grpcPort = *(*config)["port_grpc"].get<std::string>("port");
        Env env(*this, std::move(config));

        Account A1{"A1"};
        env.fund(XRP(10000), A1);
        env.close();

        // Ledger 3 has the two txs associated with funding the account
        // All other ledgers have no txs

        auto hasTxs = [](auto res) {
            return res.second.error_code() == 0 &&
                (res.first.transactions().size() == 2) &&
                //(res.transactions()[0u].transaction().has_account_set()) &&
                (res.first.transactions()[1u].transaction().has_payment());
        };
        auto noTxs = [](auto res) {
            return res.second.error_code() == 0 &&
                (res.first.transactions().size() == 0);
        };

        auto isErr = [](auto res, auto expect) {
            return res.second.error_code() == expect;
        };

        BEAST_EXPECT(
            isErr(next(grpcPort, env, ""), grpc::StatusCode::INVALID_ARGUMENT));

        BEAST_EXPECT(isErr(
            next(grpcPort, env, "0xDEADBEEF"),
            grpc::StatusCode::INVALID_ARGUMENT));

        BEAST_EXPECT(hasTxs(next(grpcPort, env, A1.human())));

        // Ledger min/max index
        {
            BEAST_EXPECT(hasTxs(next(grpcPort, env, A1.human())));

            BEAST_EXPECT(hasTxs(next(grpcPort, env, A1.human(), 0, 100)));

            BEAST_EXPECT(noTxs(next(grpcPort, env, A1.human(), 1, 2)));

            BEAST_EXPECT(isErr(
                next(grpcPort, env, A1.human(), 2, 1),
                grpc::StatusCode::INVALID_ARGUMENT));
        }

        // Ledger index min only
        {
            BEAST_EXPECT(hasTxs(next(grpcPort, env, A1.human(), -1)));

            BEAST_EXPECT(hasTxs(next(grpcPort, env, A1.human(), 1)));

            BEAST_EXPECT(isErr(
                next(grpcPort, env, A1.human(), env.current()->info().seq),
                grpc::StatusCode::INVALID_ARGUMENT));
        }

        // Ledger index max only
        {
            BEAST_EXPECT(hasTxs(next(grpcPort, env, A1.human(), -1, -1)));

            BEAST_EXPECT(hasTxs(next(
                grpcPort, env, A1.human(), -1, env.current()->info().seq)));

            BEAST_EXPECT(hasTxs(
                next(grpcPort, env, A1.human(), -1, env.closed()->info().seq)));

            BEAST_EXPECT(noTxs(next(
                grpcPort, env, A1.human(), -1, env.closed()->info().seq - 1)));
        }
        // Ledger Sequence
        {
            BEAST_EXPECT(hasTxs(nextWithSeq(
                grpcPort, env, A1.human(), env.closed()->info().seq)));

            BEAST_EXPECT(noTxs(nextWithSeq(
                grpcPort, env, A1.human(), env.closed()->info().seq - 1)));

            BEAST_EXPECT(isErr(
                nextWithSeq(
                    grpcPort, env, A1.human(), env.current()->info().seq),
                grpc::StatusCode::INVALID_ARGUMENT));

            BEAST_EXPECT(isErr(
                nextWithSeq(
                    grpcPort, env, A1.human(), env.current()->info().seq + 1),
                grpc::StatusCode::NOT_FOUND));
        }

        // Ledger Hash
        {
            BEAST_EXPECT(hasTxs(nextWithHash(
                grpcPort, env, A1.human(), env.closed()->info().hash)));

            BEAST_EXPECT(noTxs(nextWithHash(
                grpcPort, env, A1.human(), env.closed()->info().parentHash)));
        }
    }

    struct TxCheck
    {
        uint32_t sequence;
        uint32_t ledgerIndex;
        std::string hash;
        std::function<bool(org::xrpl::rpc::v1::Transaction const& res)>
            checkTxn;
    };

    void
    testAccountTxContentsGrpc()
    {
        testcase("Test AccountTx context grpc");
        // Get results for all transaction types that can be associated
        // with an account.  Start by generating all transaction types.
        using namespace test::jtx;
        using namespace std::chrono_literals;

        std::unique_ptr<Config> config = envconfig(addGrpcConfig);
        std::string grpcPort = *(*config)["port_grpc"].get<std::string>("port");
        Env env(*this, std::move(config));
        // Set time to this value (or greater) to get delivered_amount in meta
        env.timeKeeper().set(NetClock::time_point{446000001s});
        Account const alice{"alice"};
        Account const alie{"alie"};
        Account const gw{"gw"};
        auto const USD{gw["USD"]};

        std::vector<std::shared_ptr<STTx const>> txns;

        env.fund(XRP(1000000), alice, gw);
        env.close();

        // AccountSet
        env(noop(alice));

        txns.emplace_back(env.tx());
        // Payment
        env(pay(alice, gw, XRP(100)), stag(42), dtag(24), last_ledger_seq(20));

        txns.emplace_back(env.tx());
        // Regular key set
        env(regkey(alice, alie));
        env.close();

        txns.emplace_back(env.tx());
        // Trust and Offers
        env(trust(alice, USD(200)), sig(alie));

        txns.emplace_back(env.tx());
        std::uint32_t const offerSeq{env.seq(alice)};
        env(offer(alice, USD(50), XRP(150)), sig(alie));

        txns.emplace_back(env.tx());
        env.close();

        {
            Json::Value cancelOffer;
            cancelOffer[jss::Account] = alice.human();
            cancelOffer[jss::OfferSequence] = offerSeq;
            cancelOffer[jss::TransactionType] = jss::OfferCancel;
            env(cancelOffer, sig(alie));
        }
        env.close();

        txns.emplace_back(env.tx());

        // SignerListSet
        env(signers(alice, 1, {{"bogie", 1}, {"demon", 1}, {gw, 1}}),
            sig(alie));

        txns.emplace_back(env.tx());
        // Escrow
        {
            // Create an escrow.  Requires either a CancelAfter or FinishAfter.
            auto escrow = [](Account const& account,
                             Account const& to,
                             STAmount const& amount) {
                Json::Value escro;
                escro[jss::TransactionType] = jss::EscrowCreate;
                escro[jss::Flags] = tfUniversal;
                escro[jss::Account] = account.human();
                escro[jss::Destination] = to.human();
                escro[jss::Amount] = amount.getJson(JsonOptions::none);
                return escro;
            };

            NetClock::time_point const nextTime{env.now() + 2s};

            Json::Value escrowWithFinish{escrow(alice, alice, XRP(500))};
            escrowWithFinish[sfFinishAfter.jsonName] =
                nextTime.time_since_epoch().count();

            std::uint32_t const escrowFinishSeq{env.seq(alice)};
            env(escrowWithFinish, sig(alie));

            txns.emplace_back(env.tx());
            Json::Value escrowWithCancel{escrow(alice, alice, XRP(500))};
            escrowWithCancel[sfFinishAfter.jsonName] =
                nextTime.time_since_epoch().count();
            escrowWithCancel[sfCancelAfter.jsonName] =
                nextTime.time_since_epoch().count() + 1;

            std::uint32_t const escrowCancelSeq{env.seq(alice)};
            env(escrowWithCancel, sig(alie));
            env.close();

            txns.emplace_back(env.tx());
            {
                Json::Value escrowFinish;
                escrowFinish[jss::TransactionType] = jss::EscrowFinish;
                escrowFinish[jss::Flags] = tfUniversal;
                escrowFinish[jss::Account] = alice.human();
                escrowFinish[sfOwner.jsonName] = alice.human();
                escrowFinish[sfOfferSequence.jsonName] = escrowFinishSeq;
                env(escrowFinish, sig(alie));

                txns.emplace_back(env.tx());
            }
            {
                Json::Value escrowCancel;
                escrowCancel[jss::TransactionType] = jss::EscrowCancel;
                escrowCancel[jss::Flags] = tfUniversal;
                escrowCancel[jss::Account] = alice.human();
                escrowCancel[sfOwner.jsonName] = alice.human();
                escrowCancel[sfOfferSequence.jsonName] = escrowCancelSeq;
                env(escrowCancel, sig(alie));

                txns.emplace_back(env.tx());
            }
            env.close();
        }

        // PayChan
        {
            std::uint32_t payChanSeq{env.seq(alice)};
            Json::Value payChanCreate;
            payChanCreate[jss::TransactionType] = jss::PaymentChannelCreate;
            payChanCreate[jss::Flags] = tfUniversal;
            payChanCreate[jss::Account] = alice.human();
            payChanCreate[jss::Destination] = gw.human();
            payChanCreate[jss::Amount] =
                XRP(500).value().getJson(JsonOptions::none);
            payChanCreate[sfSettleDelay.jsonName] =
                NetClock::duration{100s}.count();
            payChanCreate[sfPublicKey.jsonName] = strHex(alice.pk().slice());
            env(payChanCreate, sig(alie));
            env.close();

            txns.emplace_back(env.tx());
            std::string const payChanIndex{
                strHex(keylet::payChan(alice, gw, payChanSeq).key)};

            {
                Json::Value payChanFund;
                payChanFund[jss::TransactionType] = jss::PaymentChannelFund;
                payChanFund[jss::Flags] = tfUniversal;
                payChanFund[jss::Account] = alice.human();
                payChanFund[sfPayChannel.jsonName] = payChanIndex;
                payChanFund[jss::Amount] =
                    XRP(200).value().getJson(JsonOptions::none);
                env(payChanFund, sig(alie));
                env.close();

                txns.emplace_back(env.tx());
            }
            {
                Json::Value payChanClaim;
                payChanClaim[jss::TransactionType] = jss::PaymentChannelClaim;
                payChanClaim[jss::Flags] = tfClose;
                payChanClaim[jss::Account] = gw.human();
                payChanClaim[sfPayChannel.jsonName] = payChanIndex;
                payChanClaim[sfPublicKey.jsonName] = strHex(alice.pk().slice());
                env(payChanClaim);
                env.close();

                txns.emplace_back(env.tx());
            }
        }

        // Check
        {
            uint256 const aliceCheckId{getCheckIndex(alice, env.seq(alice))};
            env(check::create(alice, gw, XRP(300)), sig(alie));

            auto txn = env.tx();
            uint256 const gwCheckId{getCheckIndex(gw, env.seq(gw))};
            env(check::create(gw, alice, XRP(200)));
            env.close();

            // need to switch the order of the previous 2 txns, since they are
            // in the same ledger and account_tx returns them in a different
            // order
            txns.emplace_back(env.tx());
            txns.emplace_back(txn);
            env(check::cash(alice, gwCheckId, XRP(200)), sig(alie));

            txns.emplace_back(env.tx());
            env(check::cancel(alice, aliceCheckId), sig(alie));

            txns.emplace_back(env.tx());
            env.close();
        }

        // Deposit preauthorization.
        env(deposit::auth(alice, gw), sig(alie));
        env.close();

        txns.emplace_back(env.tx());
        // Multi Sig with memo
        auto const baseFee = env.current()->fees().base;
        env(noop(alice),
            msig(gw),
            fee(2 * baseFee),
            memo("data", "format", "type"));
        env.close();

        txns.emplace_back(env.tx());
        // Setup is done.  Look at the transactions returned by account_tx.

        static const TxCheck txCheck[]{
            {21,
             15,
             "7B71E7F4B6B8D11794E90C05E1A339E4DDA59F0415A040F84BC5D81C26BB71D7",
             [this](auto res) {
                 return BEAST_EXPECT(res.has_account_set()) &&
                     BEAST_EXPECT(res.has_fee()) &&
                     BEAST_EXPECT(res.fee().drops() == 20) &&
                     BEAST_EXPECT(res.memos_size() == 1) &&
                     BEAST_EXPECT(res.memos(0).has_memo_data()) &&
                     BEAST_EXPECT(res.memos(0).memo_data().value() == "data") &&
                     BEAST_EXPECT(res.memos(0).has_memo_format()) &&
                     BEAST_EXPECT(
                            res.memos(0).memo_format().value() == "format") &&
                     BEAST_EXPECT(res.memos(0).has_memo_type()) &&
                     BEAST_EXPECT(res.memos(0).memo_type().value() == "type") &&
                     BEAST_EXPECT(res.has_signing_public_key()) &&
                     BEAST_EXPECT(res.signing_public_key().value() == "") &&
                     BEAST_EXPECT(res.signers_size() == 1) &&
                     BEAST_EXPECT(res.signers(0).has_account()) &&
                     BEAST_EXPECT(
                            res.signers(0).account().value().address() ==
                            "rExKpRKXNz25UAjbckCRtQsJFcSfjL9Er3") &&
                     BEAST_EXPECT(res.signers(0).has_transaction_signature()) &&
                     BEAST_EXPECT(
                            strHex(res.signers(0)
                                       .transaction_signature()
                                       .value()) ==
                            "3044022018CAA75EC1D9C5993075BA2CE1817C5378E6A365B7"
                            "A0B00B7720266D55DA18740220578A5F9693AC6CBE9E128E52"
                            "89C38197D0410958FC949FA2DAFB176BB3A3964D") &&
                     BEAST_EXPECT(res.signers(0).has_signing_public_key()) &&
                     BEAST_EXPECT(
                            strHex(
                                res.signers(0).signing_public_key().value()) ==
                            "038FDA65CFAADC4571D1C5AA5ADF0F881FCDBFD57B70714714"
                            "EB38BEE2553CA33A"); /*TODO memos and signers*/
             }},
            {20,
             14,
             "8D063704F3B3E3F9EF97804371AEF4BBBE615F3A0A3E7C22704C5841998A0D3D",
             [this](auto res) {
                 return BEAST_EXPECT(res.has_deposit_preauth()) &&
                     BEAST_EXPECT(
                            res.deposit_preauth()
                                .authorize()
                                .value()
                                .address() ==
                            "rExKpRKXNz25UAjbckCRtQsJFcSfjL9Er3");
             }},
            {19,
             13,
             "69951DF2ABB18CA830206811BEB95D87B019209B1281C3C2E1F63D3CDC888B4F",
             [this](auto res) {
                 return BEAST_EXPECT(res.has_check_cancel()) &&
                     BEAST_EXPECT(
                            strHex(res.check_cancel().check_id().value()) ==
                            "0047B03EBBB0E4A2F93290DAFD8969F65655F6AA913351469B"
                            "1516D88"
                            "E5C9AE9");
             }},
            {18,
             13,
             "D97D44D2AA611A40D902B017907261C298152FB3E6E95376E9C997C2DFD0A108",
             [this](auto res) {
                 return BEAST_EXPECT(res.has_check_cash()) &&
                     BEAST_EXPECT(
                            strHex(res.check_cash().check_id().value()) ==
                            "359E11CA7A773F9BB0A670D2607DA36937B66583CF1A061189"
                            "62A831B"
                            "4654363") &&
                     BEAST_EXPECT(res.check_cash()
                                      .amount()
                                      .value()
                                      .has_xrp_amount()) &&
                     BEAST_EXPECT(
                            res.check_cash()
                                .amount()
                                .value()
                                .xrp_amount()
                                .drops() == 200000000);
             }},
            {17,
             12,
             "8404599D719703DAB563107DBF0B41FCEA65D84072B20C5F392B665794DC64F6",
             [this](auto res) {
                 return BEAST_EXPECT(res.has_check_create()) &&
                     BEAST_EXPECT(
                            res.check_create()
                                .destination()
                                .value()
                                .address() ==
                            "rExKpRKXNz25UAjbckCRtQsJFcSfjL9Er3") &&
                     BEAST_EXPECT(res.check_create()
                                      .send_max()
                                      .value()
                                      .has_xrp_amount()) &&
                     BEAST_EXPECT(
                            res.check_create()
                                .send_max()
                                .value()
                                .xrp_amount()
                                .drops() == 300000000);
             }},
            {5,
             12,
             "5AD812CB1C71DD10BDC854FE791633BC389A6FB08F6CC154EE8E29276D0F1DD8",
             [this](auto res) {
                 return BEAST_EXPECT(res.has_check_create()) &&
                     BEAST_EXPECT(
                            res.check_create()
                                .destination()
                                .value()
                                .address() ==
                            "rG1QQv2nh2gr7RCZ1P8YYcBUKCCN633jCn") &&
                     BEAST_EXPECT(res.check_create()
                                      .send_max()
                                      .value()
                                      .has_xrp_amount()) &&
                     BEAST_EXPECT(
                            res.check_create()
                                .send_max()
                                .value()
                                .xrp_amount()
                                .drops() == 200000000);
             }},
            {4,
             11,
             "766A206FF6FAD41F533EBA4C7F5146BB450DCA7C106559D9C7075F3F9AD04BEA",
             [this](auto res) {
                 return BEAST_EXPECT(res.has_payment_channel_claim()) &&
                     BEAST_EXPECT(
                            strHex(res.payment_channel_claim()
                                       .channel()
                                       .value()) ==
                            "1C3B03E7B8CC3369BBBBF780DD08E9F4A024687350B420F56F"
                            "E8E67C1"
                            "689E2FB") &&
                     BEAST_EXPECT(
                            strHex(res.payment_channel_claim()
                                       .public_key()
                                       .value()) ==
                            "0388935426E0D08083314842EDFBB2D517BD47699F9A452731"
                            "8A8E104"
                            "68C97C052");
             }},
            {16,
             10,
             "CE97C5D8BED19043668FF7682BBB5568A19891CA9EE70797C8EB079B65B5234B",
             [this](auto res) {
                 return BEAST_EXPECT(res.has_payment_channel_fund()) &&
                     BEAST_EXPECT(
                            strHex(
                                res.payment_channel_fund().channel().value()) ==
                            "1C3B03E7B8CC3369BBBBF780DD08E9F4A024687350B420F56F"
                            "E8E67C1"
                            "689E2FB") &&
                     BEAST_EXPECT(res.payment_channel_fund()
                                      .amount()
                                      .value()
                                      .has_xrp_amount()) &&
                     BEAST_EXPECT(
                            res.payment_channel_fund()
                                .amount()
                                .value()
                                .xrp_amount()
                                .drops() == 200000000);
             }},
            {15,
             9,
             "50B9472E2BA855BD7C7EABFA653081E12075357559D3E1D4875ECEC52ADD441A",
             [this](auto res) {
                 return BEAST_EXPECT(res.has_payment_channel_create()) &&
                     BEAST_EXPECT(res.payment_channel_create()
                                      .amount()
                                      .value()
                                      .has_xrp_amount()) &&
                     BEAST_EXPECT(
                            res.payment_channel_create()
                                .amount()
                                .value()
                                .xrp_amount()
                                .drops() == 500000000) &&
                     BEAST_EXPECT(
                            res.payment_channel_create()
                                .destination()
                                .value()
                                .address() ==
                            "rExKpRKXNz25UAjbckCRtQsJFcSfjL9Er3") &&
                     BEAST_EXPECT(
                            res.payment_channel_create()
                                .settle_delay()
                                .value() == 100) &&
                     BEAST_EXPECT(
                            strHex(res.payment_channel_create()
                                       .public_key()
                                       .value()) ==
                            "0388935426E0D08083314842EDFBB2D517BD47699F9A452731"
                            "8A8E104"
                            "68C97C052");
             }},
            {14,
             8,
             "94E30771C8F0227F37BAADB4D1C4772E7191608E781B7D27BE9318D57B16847D",
             [this](auto res) {
                 return BEAST_EXPECT(res.has_escrow_cancel()) &&
                     BEAST_EXPECT(
                            res.escrow_cancel().owner().value().address() ==
                            "rG1QQv2nh2gr7RCZ1P8YYcBUKCCN633jCn") &&
                     BEAST_EXPECT(
                            res.escrow_cancel().offer_sequence().value() == 12

                     );
             }},
            {13,
             8,
             "643431B1A3FC92878555C511659ED5017B571ACBE2DF0F8FDF4A7AF56B279FF5",
             [this](auto res) {
                 return BEAST_EXPECT(res.has_escrow_finish()) &&
                     BEAST_EXPECT(
                            res.escrow_finish().owner().value().address() ==
                            "rG1QQv2nh2gr7RCZ1P8YYcBUKCCN633jCn") &&
                     BEAST_EXPECT(
                            res.escrow_finish().offer_sequence().value() == 11

                     );
             }},
            {12,
             7,
             "B8D01F3CEDBC00493CFF311A281D61DB36C7832700C066D0F7495FAE1D5157C1",
             [this](auto res) {
                 return BEAST_EXPECT(res.has_escrow_create()) &&
                     BEAST_EXPECT(res.escrow_create()
                                      .amount()
                                      .value()
                                      .has_xrp_amount()) &&
                     BEAST_EXPECT(
                            res.escrow_create()
                                .amount()
                                .value()
                                .xrp_amount()
                                .drops() == 500000000) &&
                     BEAST_EXPECT(
                            res.escrow_create()
                                .destination()
                                .value()
                                .address() ==
                            "rG1QQv2nh2gr7RCZ1P8YYcBUKCCN633jCn") &&
                     BEAST_EXPECT(
                            res.escrow_create().cancel_after().value() ==
                            446000133) &&
                     BEAST_EXPECT(
                            res.escrow_create().finish_after().value() ==
                            446000132);
             }},
            {11,
             7,
             "9C3B730E82298B74CEA8A2A8E9903946A8D4D2C41029B829CFC22F401746DBD4",
             [this](auto res) {
                 return BEAST_EXPECT(res.has_escrow_create()) &&
                     BEAST_EXPECT(res.escrow_create()
                                      .amount()
                                      .value()
                                      .has_xrp_amount()) &&
                     BEAST_EXPECT(
                            res.escrow_create()
                                .amount()
                                .value()
                                .xrp_amount()
                                .drops() == 500000000) &&
                     BEAST_EXPECT(
                            res.escrow_create()
                                .destination()
                                .value()
                                .address() ==
                            "rG1QQv2nh2gr7RCZ1P8YYcBUKCCN633jCn") &&
                     BEAST_EXPECT(
                            res.escrow_create().finish_after().value() ==
                            446000132);
             }},
            {10,
             7,
             "3E5C670AFC56E478A715B722BA6406283AAD11C32E1813DDBAEA52E2C2822BB1",
             [this](auto res) {
                 return BEAST_EXPECT(res.has_signer_list_set()) &&
                     BEAST_EXPECT(
                            res.signer_list_set().signer_quorum().value() ==
                            1) &&
                     BEAST_EXPECT(
                            res.signer_list_set().signer_entries().size() ==
                            3) &&
                     BEAST_EXPECT(
                            res.signer_list_set()
                                .signer_entries()[0]
                                .account()
                                .value()
                                .address() ==
                            "rXZVaSDvesEDh9bstf6Vw36XKGi7B35kw") &&
                     BEAST_EXPECT(
                            res.signer_list_set()
                                .signer_entries()[0]
                                .signer_weight()
                                .value() == 1) &&
                     BEAST_EXPECT(
                            res.signer_list_set()
                                .signer_entries()[1]
                                .account()
                                .value()
                                .address() ==
                            "rharXKno1ZNYDDeNmsYva3e79J956edxrZ") &&
                     BEAST_EXPECT(
                            res.signer_list_set()
                                .signer_entries()[1]
                                .signer_weight()
                                .value() == 1) &&
                     BEAST_EXPECT(
                            res.signer_list_set()
                                .signer_entries()[2]
                                .account()
                                .value()
                                .address() ==
                            "rExKpRKXNz25UAjbckCRtQsJFcSfjL9Er3") &&
                     BEAST_EXPECT(
                            res.signer_list_set()
                                .signer_entries()[2]
                                .signer_weight()
                                .value() == 1

                     );
             }},
            {9,
             6,
             "6244B46A29C902C23C2826C9ED9E6D4744F852E304F17039A2F9C7482BA21DBC",
             [this](auto res) {
                 return BEAST_EXPECT(res.has_offer_cancel()) &&
                     BEAST_EXPECT(
                            res.offer_cancel().offer_sequence().value() == 8);
             }},
            {8,
             5,
             "770C01548178D79D286B8BEC0D061934DB0D8396798B6BC2F9719FF9BFB112AD",
             [this](auto res) {
                 return BEAST_EXPECT(res.has_offer_create()) &&
                     BEAST_EXPECT(res.offer_create()
                                      .taker_gets()
                                      .value()
                                      .has_xrp_amount()) &&
                     BEAST_EXPECT(
                            res.offer_create()
                                .taker_gets()
                                .value()
                                .xrp_amount()
                                .drops() == 150000000) &&
                     BEAST_EXPECT(res.offer_create()
                                      .taker_pays()
                                      .value()
                                      .has_issued_currency_amount()) &&
                     BEAST_EXPECT(
                            res.offer_create()
                                .taker_pays()
                                .value()
                                .issued_currency_amount()
                                .currency()
                                .name() == "USD") &&
                     BEAST_EXPECT(
                            res.offer_create()
                                .taker_pays()
                                .value()
                                .issued_currency_amount()
                                .value() == "50") &&
                     BEAST_EXPECT(
                            res.offer_create()
                                .taker_pays()
                                .value()
                                .issued_currency_amount()
                                .issuer()
                                .address() ==
                            "rExKpRKXNz25UAjbckCRtQsJFcSfjL9Er3");
             }},
            {7,
             5,
             "F55F557D78BB867BD34EBA10CF4C58C1CC18EE9CE1E0DB32B4881E9DB7A7F3A5",
             [this](auto res) {
                 return BEAST_EXPECT(res.has_trust_set()) &&
                     BEAST_EXPECT(res.trust_set()
                                      .limit_amount()
                                      .value()
                                      .has_issued_currency_amount()) &&
                     BEAST_EXPECT(
                            res.trust_set()
                                .limit_amount()
                                .value()
                                .issued_currency_amount()
                                .currency()
                                .name() == "USD") &&
                     BEAST_EXPECT(
                            res.trust_set()
                                .limit_amount()
                                .value()
                                .issued_currency_amount()
                                .value() == "200") &&
                     BEAST_EXPECT(
                            res.trust_set()
                                .limit_amount()
                                .value()
                                .issued_currency_amount()
                                .issuer()
                                .address() ==
                            "rExKpRKXNz25UAjbckCRtQsJFcSfjL9Er3");
             }},
            {6,
             4,
             "2C6290DA2B6D3696752D2F8A2F684971FCC1F88F557CA8D44AC4D63759BFFB17",
             [this](auto res) {
                 return BEAST_EXPECT(res.has_set_regular_key()) &&
                     BEAST_EXPECT(
                            res.set_regular_key()
                                .regular_key()
                                .value()
                                .address() ==
                            "r91N98XHbq8RqAXQa5mQcfwrFTMa2fnkxV");
             }},
            {5,
             4,
             "A9FE93A51361ED922E039F8C77E15E672465AD41DADACB101DF59E48145D005F",
             [this](auto res) {
                 return BEAST_EXPECT(res.has_payment()) &&
                     BEAST_EXPECT(
                            res.payment().amount().value().has_xrp_amount()) &&
                     BEAST_EXPECT(
                            res.payment()
                                .amount()
                                .value()
                                .xrp_amount()
                                .drops() == 100000000) &&
                     BEAST_EXPECT(
                            res.payment().destination().value().address() ==
                            "rExKpRKXNz25UAjbckCRtQsJFcSfjL9Er3") &&
                     BEAST_EXPECT(res.has_source_tag()) &&
                     BEAST_EXPECT(res.source_tag().value() == 42) &&
                     BEAST_EXPECT(res.payment().has_destination_tag()) &&
                     BEAST_EXPECT(
                            res.payment().destination_tag().value() == 24) &&
                     BEAST_EXPECT(res.has_last_ledger_sequence()) &&
                     BEAST_EXPECT(res.last_ledger_sequence().value() == 20) &&
                     BEAST_EXPECT(res.has_transaction_signature()) &&
                     BEAST_EXPECT(res.has_account()) &&
                     BEAST_EXPECT(
                            res.account().value().address() ==
                            "rG1QQv2nh2gr7RCZ1P8YYcBUKCCN633jCn") &&
                     BEAST_EXPECT(res.has_flags()) &&
                     BEAST_EXPECT(res.flags().value() == 2147483648);
             }},
            {4,
             4,
             "782A3A3D00C05A10C3193E2EE161CCCF76E95FCF7B3FBA7427CDF69F7D22D59E",
             [this](auto res) { return res.has_account_set(); }},
            {3,
             3,
             "9CE54C3B934E473A995B477E92EC229F99CED5B62BF4D2ACE4DC42719103AE2F",
             [this](auto res) {
                 return BEAST_EXPECT(res.has_account_set()) &&
                     BEAST_EXPECT(res.account_set().set_flag().value() == 8);
             }},
            {1,
             3,
             "2B5054734FA43C6C7B54F61944FAD6178ACD5D0272B39BA7FCD32A5D3932FBFF",
             [this](auto res) {
                 return BEAST_EXPECT(res.has_payment()) &&
                     BEAST_EXPECT(
                            res.payment().amount().value().has_xrp_amount()) &&
                     BEAST_EXPECT(
                            res.payment()
                                .amount()
                                .value()
                                .xrp_amount()
                                .drops() == 1000000000010) &&
                     BEAST_EXPECT(
                            res.payment().destination().value().address() ==
                            "rG1QQv2nh2gr7RCZ1P8YYcBUKCCN633jCn") &&
                     BEAST_EXPECT(res.has_transaction_signature()) &&
                     BEAST_EXPECT(
                            strHex(res.transaction_signature().value()) ==

                            "30440220474CC4207C1AF5B54092876C1A62E3314CE92455F7"
                            "C43F723CD0BE8F0CD0158002201D87D6F4292F27325FF2E40C"
                            "77977BD3DD97A782FECA6558BC83D282921B3AB6") &&
                     BEAST_EXPECT(res.has_signing_public_key()) &&
                     BEAST_EXPECT(
                            strHex(res.signing_public_key().value()) ==
                            "0330E7FC9D56BB25D6893BA3F317AE5BCF33B3291BD63DB326"
                            "54A313222F7FD020");
             }},
        };

        using MetaCheck =
            std::function<bool(org::xrpl::rpc::v1::Meta const& res)>;
        static const MetaCheck txMetaCheck[]{
            {[this](auto meta) {
                return BEAST_EXPECT(meta.transaction_index() == 0) &&
                    BEAST_EXPECT(meta.affected_nodes_size() == 1) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](org::xrpl::rpc::v1::AffectedNode const&
                                      entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ACCOUNT_ROOT;
                               }) == 1);
            }},
            {[this](auto meta) {
                return BEAST_EXPECT(meta.transaction_index() == 0) &&
                    BEAST_EXPECT(meta.affected_nodes_size() == 3) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_DEPOSIT_PREAUTH;
                               }) == 1) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),

                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ACCOUNT_ROOT;
                               }) == 1) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_DIRECTORY_NODE;
                               }) == 1);
            }},
            {[this](auto meta) {
                return BEAST_EXPECT(meta.transaction_index() == 1) &&
                    BEAST_EXPECT(meta.affected_nodes_size() == 5) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_CHECK;
                               }) == 1) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_DIRECTORY_NODE;
                               }) == 2) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ACCOUNT_ROOT;
                               }) == 2);
            }},
            {[this](auto meta) {
                return BEAST_EXPECT(meta.transaction_index() == 0) &&
                    BEAST_EXPECT(meta.affected_nodes_size() == 5) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_CHECK;
                               }) == 1) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_DIRECTORY_NODE;
                               }) == 2) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ACCOUNT_ROOT;
                               }) == 2);
            }},
            {[this](auto meta) {
                return BEAST_EXPECT(meta.transaction_index() == 1) &&
                    BEAST_EXPECT(meta.affected_nodes_size() == 5) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_CHECK;
                               }) == 1) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_DIRECTORY_NODE;
                               }) == 2) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ACCOUNT_ROOT;
                               }) == 2);
            }},
            {[this](auto meta) {
                return BEAST_EXPECT(meta.transaction_index() == 0) &&
                    BEAST_EXPECT(meta.affected_nodes_size() == 5) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_CHECK;
                               }) == 1) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_DIRECTORY_NODE;
                               }) == 2) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ACCOUNT_ROOT;
                               }) == 2);
            }},
            {[this](auto meta) {
                return BEAST_EXPECT(meta.transaction_index() == 0) &&
                    BEAST_EXPECT(meta.affected_nodes_size() == 5) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_PAY_CHANNEL;
                               }) == 1) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_DIRECTORY_NODE;
                               }) == 2) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ACCOUNT_ROOT;
                               }) == 2);
            }},
            {[this](auto meta) {
                return BEAST_EXPECT(meta.transaction_index() == 0) &&
                    BEAST_EXPECT(meta.affected_nodes_size() == 2) &&

                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_PAY_CHANNEL;
                               }) == 1) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ACCOUNT_ROOT;
                               }) == 1);
            }},
            {[this](auto meta) {
                return BEAST_EXPECT(meta.transaction_index() == 0) &&
                    BEAST_EXPECT(meta.affected_nodes_size() == 5) &&

                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_PAY_CHANNEL;
                               }) == 1) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_DIRECTORY_NODE;
                               }) == 2) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ACCOUNT_ROOT;
                               }) == 2);
            }},
            {[this](auto meta) {
                return BEAST_EXPECT(meta.transaction_index() == 1) &&
                    BEAST_EXPECT(meta.affected_nodes_size() == 3) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ESCROW;
                               }) == 1) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ACCOUNT_ROOT;
                               }) == 1) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_DIRECTORY_NODE;
                               }) == 1);
            }},
            {[this](auto meta) {
                return BEAST_EXPECT(meta.transaction_index() == 0) &&
                    BEAST_EXPECT(meta.affected_nodes_size() == 3) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ESCROW;
                               }) == 1) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ACCOUNT_ROOT;
                               }) == 1) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_DIRECTORY_NODE;
                               }) == 1);
            }},
            {[this](auto meta) {
                return BEAST_EXPECT(meta.transaction_index() == 2) &&
                    BEAST_EXPECT(meta.affected_nodes_size() == 3) &&

                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ESCROW;
                               }) == 1) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ACCOUNT_ROOT;
                               }) == 1) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_DIRECTORY_NODE;
                               }) == 1);
            }},
            {[this](auto meta) {
                return BEAST_EXPECT(meta.transaction_index() == 1) &&
                    BEAST_EXPECT(meta.affected_nodes_size() == 3) &&

                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ESCROW;
                               }) == 1) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ACCOUNT_ROOT;
                               }) == 1) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_DIRECTORY_NODE;
                               }) == 1);
            }},
            {[this](auto meta) {
                return BEAST_EXPECT(meta.transaction_index() == 0) &&
                    BEAST_EXPECT(meta.affected_nodes_size() == 3) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_SIGNER_LIST;
                               }) == 1) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ACCOUNT_ROOT;
                               }) == 1) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_DIRECTORY_NODE;
                               }) == 1);
            }},
            {[this](auto meta) {
                return BEAST_EXPECT(meta.transaction_index() == 0) &&
                    BEAST_EXPECT(meta.affected_nodes_size() == 4) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_DIRECTORY_NODE;
                               }) == 2) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ACCOUNT_ROOT;
                               }) == 1) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_OFFER;
                               }) == 1);
            }},
            {[this](auto meta) {
                return BEAST_EXPECT(meta.transaction_index() == 1) &&
                    BEAST_EXPECT(meta.affected_nodes_size() == 4) &&

                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_DIRECTORY_NODE;
                               }) == 2) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ACCOUNT_ROOT;
                               }) == 1) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_OFFER;
                               }) == 1);
            }},
            {[this](auto meta) {
                return BEAST_EXPECT(meta.transaction_index() == 0) &&
                    BEAST_EXPECT(meta.affected_nodes_size() == 5) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_DIRECTORY_NODE;
                               }) == 2) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ACCOUNT_ROOT;
                               }) == 2) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_RIPPLE_STATE;
                               }) == 1);
            }},
            {[this](auto meta) {
                return BEAST_EXPECT(meta.transaction_index() == 2) &&
                    BEAST_EXPECT(meta.affected_nodes_size() == 1) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ACCOUNT_ROOT;
                               }) == 1);
            }},
            {[this](auto meta) {
                return BEAST_EXPECT(meta.transaction_index() == 1) &&
                    BEAST_EXPECT(meta.affected_nodes_size() == 2) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ACCOUNT_ROOT;
                               }) == 2);
            }},
            {[this](auto meta) {
                return BEAST_EXPECT(meta.transaction_index() == 0) &&
                    BEAST_EXPECT(meta.affected_nodes_size() == 1) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ACCOUNT_ROOT;
                               }) == 1);
            }},
            {[this](auto meta) {
                return BEAST_EXPECT(meta.transaction_index() == 2) &&
                    BEAST_EXPECT(meta.affected_nodes_size() == 1) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ACCOUNT_ROOT;
                               }) == 1);
            }},
            {[this](auto meta) {
                return BEAST_EXPECT(meta.transaction_index() == 0) &&
                    BEAST_EXPECT(meta.affected_nodes_size() == 2) &&
                    BEAST_EXPECT(
                           std::count_if(
                               meta.affected_nodes().begin(),
                               meta.affected_nodes().end(),
                               [](auto entry) {
                                   return entry.ledger_entry_type() ==
                                       org::xrpl::rpc::v1::LedgerEntryType::
                                           LEDGER_ENTRY_TYPE_ACCOUNT_ROOT;
                               }) == 2);
            }}};

        auto doCheck = [this](auto txn, auto txCheck) {
            return BEAST_EXPECT(txn.has_transaction()) &&
                BEAST_EXPECT(txn.validated()) &&
                BEAST_EXPECT(strHex(txn.hash()) == txCheck.hash) &&
                BEAST_EXPECT(txn.ledger_index() == txCheck.ledgerIndex) &&
                BEAST_EXPECT(
                       txn.transaction().sequence().value() ==
                       txCheck.sequence) &&
                txCheck.checkTxn(txn.transaction());
        };

        auto doMetaCheck = [this](auto txn, auto txMetaCheck) {
            return BEAST_EXPECT(txn.has_meta()) &&
                BEAST_EXPECT(txn.meta().has_transaction_result()) &&
                BEAST_EXPECT(
                       txn.meta().transaction_result().result_type() ==
                       org::xrpl::rpc::v1::TransactionResult::
                           RESULT_TYPE_TES) &&
                BEAST_EXPECT(
                       txn.meta().transaction_result().result() ==
                       "tesSUCCESS") &&
                txMetaCheck(txn.meta());
        };

        auto [res, status] = next(grpcPort, env, alice.human());

        if (!BEAST_EXPECT(status.error_code() == 0))
            return;

        if (!BEAST_EXPECT(
                res.transactions().size() ==
                std::extent<decltype(txCheck)>::value))
            return;
        for (int i = 0; i < res.transactions().size(); ++i)
        {
            BEAST_EXPECT(doCheck(res.transactions()[i], txCheck[i]));
            BEAST_EXPECT(doMetaCheck(res.transactions()[i], txMetaCheck[i]));
        }

        std::tie(res, status) = nextBinary(grpcPort, env, alice.human());

        // txns vector does not contain the first two transactions returned by
        // account_tx
        if (!BEAST_EXPECT(res.transactions().size() == txns.size() + 2))
            return;

        std::reverse(txns.begin(), txns.end());
        for (int i = 0; i < txns.size(); ++i)
        {
            auto toByteString = [](auto data) {
                const char* bytes = reinterpret_cast<const char*>(data.data());
                return std::string(bytes, data.size());
            };

            auto tx = txns[i];
            Serializer s = tx->getSerializer();
            std::string bin = toByteString(s);
            BEAST_EXPECT(res.transactions(i).transaction_binary() == bin);
        }
    }

    void
    testAccountTxPagingGrpc()
    {
        testcase("Test Account_tx Grpc");

        using namespace test::jtx;
        std::unique_ptr<Config> config = envconfig(addGrpcConfig);
        std::string grpcPort = *(*config)["port_grpc"].get<std::string>("port");
        Env env(*this, std::move(config));

        Account A1{"A1"};
        Account A2{"A2"};
        Account A3{"A3"};

        env.fund(XRP(10000), A1, A2, A3);
        env.close();

        env.trust(A3["USD"](1000), A1);
        env.trust(A2["USD"](1000), A1);
        env.trust(A3["USD"](1000), A2);
        env.close();

        for (auto i = 0; i < 5; ++i)
        {
            env(pay(A2, A1, A2["USD"](2)));
            env(pay(A3, A1, A3["USD"](2)));
            env(offer(A1, XRP(11), A1["USD"](1)));
            env(offer(A2, XRP(10), A2["USD"](1)));
            env(offer(A3, XRP(9), A3["USD"](1)));
            env.close();
        }

        /* The sequence/ledger for A3 are as follows:
         * seq     ledger_index
         * 3  ----> 3
         * 1  ----> 3
         * 2  ----> 4
         * 2  ----> 4
         * 2  ----> 5
         * 3  ----> 5
         * 4  ----> 6
         * 5  ----> 6
         * 6  ----> 7
         * 7  ----> 7
         * 8  ----> 8
         * 9  ----> 8
         * 10 ----> 9
         * 11 ----> 9
         */

        // page through the results in several ways.
        {
            // limit = 2, 3 batches giving the first 6 txs
            auto [res, status] = next(grpcPort, env, A3.human(), 2, 5, 2, true);

            auto txs = res.transactions();
            if (!BEAST_EXPECT(txs.size() == 2))
                return;

            BEAST_EXPECT(checkTransaction(txs[0u], 3, 3));
            BEAST_EXPECT(checkTransaction(txs[1u], 3, 3));
            if (!BEAST_EXPECT(res.has_marker()))
                return;

            std::tie(res, status) = next(
                grpcPort, env, A3.human(), 2, 5, 2, true, res.mutable_marker());
            txs = res.transactions();
            if (!BEAST_EXPECT(txs.size() == 2))
                return;
            BEAST_EXPECT(checkTransaction(txs[0u], 4, 4));
            BEAST_EXPECT(checkTransaction(txs[1u], 4, 4));
            if (!BEAST_EXPECT(res.has_marker()))
                return;

            std::tie(res, status) = next(
                grpcPort, env, A3.human(), 2, 5, 2, true, res.mutable_marker());
            txs = res.transactions();
            if (!BEAST_EXPECT(txs.size() == 2))
                return;
            BEAST_EXPECT(checkTransaction(txs[0u], 4, 5));
            BEAST_EXPECT(checkTransaction(txs[1u], 5, 5));
            BEAST_EXPECT(!res.has_marker());
            return;
        }

        {
            // limit 1, 3 requests giving the first 3 txs
            auto [res, status] = next(grpcPort, env, A3.human(), 3, 9, 1, true);
            auto txs = res.transactions();
            if (!BEAST_EXPECT(txs.size() == 1))
                return;
            BEAST_EXPECT(checkTransaction(txs[0u], 3, 3));
            if (!BEAST_EXPECT(res.has_marker()))
                return;

            std::tie(res, status) = next(
                grpcPort, env, A3.human(), 3, 9, 1, true, res.mutable_marker());
            txs = res.transactions();
            if (!BEAST_EXPECT(txs.size() == 1))
                return;
            BEAST_EXPECT(checkTransaction(txs[0u], 3, 3));
            if (!BEAST_EXPECT(res.has_marker()))
                return;

            std::tie(res, status) = next(
                grpcPort, env, A3.human(), 3, 9, 1, true, res.mutable_marker());
            txs = res.transactions();
            if (!BEAST_EXPECT(txs.size() == 1))
                return;
            BEAST_EXPECT(checkTransaction(txs[0u], 4, 4));
            if (!BEAST_EXPECT(res.has_marker()))
                return;

            // continue with limit 3, to end of all txs
            std::tie(res, status) = next(
                grpcPort, env, A3.human(), 3, 9, 3, true, res.mutable_marker());
            txs = res.transactions();
            if (!BEAST_EXPECT(txs.size() == 3))
                return;
            BEAST_EXPECT(checkTransaction(txs[0u], 4, 4));
            BEAST_EXPECT(checkTransaction(txs[1u], 4, 5));
            BEAST_EXPECT(checkTransaction(txs[2u], 5, 5));
            if (!BEAST_EXPECT(res.has_marker()))
                return;

            std::tie(res, status) = next(
                grpcPort, env, A3.human(), 3, 9, 3, true, res.mutable_marker());
            txs = res.transactions();
            if (!BEAST_EXPECT(txs.size() == 3))
                return;
            BEAST_EXPECT(checkTransaction(txs[0u], 6, 6));
            BEAST_EXPECT(checkTransaction(txs[1u], 7, 6));
            BEAST_EXPECT(checkTransaction(txs[2u], 8, 7));
            if (!BEAST_EXPECT(res.has_marker()))
                return;

            std::tie(res, status) = next(
                grpcPort, env, A3.human(), 3, 9, 3, true, res.mutable_marker());
            txs = res.transactions();
            if (!BEAST_EXPECT(txs.size() == 3))
                return;
            BEAST_EXPECT(checkTransaction(txs[0u], 9, 7));
            BEAST_EXPECT(checkTransaction(txs[1u], 10, 8));
            BEAST_EXPECT(checkTransaction(txs[2u], 11, 8));
            if (!BEAST_EXPECT(res.has_marker()))
                return;

            std::tie(res, status) = next(
                grpcPort, env, A3.human(), 3, 9, 3, true, res.mutable_marker());
            txs = res.transactions();
            if (!BEAST_EXPECT(txs.size() == 2))
                return;
            BEAST_EXPECT(checkTransaction(txs[0u], 12, 9));
            BEAST_EXPECT(checkTransaction(txs[1u], 13, 9));
            BEAST_EXPECT(!res.has_marker());
        }

        {
            // limit 2, descending, 2 batches giving last 4 txs
            auto [res, status] =
                next(grpcPort, env, A3.human(), 3, 9, 2, false);
            auto txs = res.transactions();
            if (!BEAST_EXPECT(txs.size() == 2))
                return;
            BEAST_EXPECT(checkTransaction(txs[0u], 13, 9));
            BEAST_EXPECT(checkTransaction(txs[1u], 12, 9));
            if (!BEAST_EXPECT(res.has_marker()))
                return;

            std::tie(res, status) = next(
                grpcPort,
                env,
                A3.human(),
                3,
                9,
                2,
                false,
                res.mutable_marker());
            txs = res.transactions();
            if (!BEAST_EXPECT(txs.size() == 2))
                return;
            BEAST_EXPECT(checkTransaction(txs[0u], 11, 8));
            BEAST_EXPECT(checkTransaction(txs[1u], 10, 8));
            if (!BEAST_EXPECT(res.has_marker()))
                return;

            // continue with limit 3 until all txs have been seen
            std::tie(res, status) = next(
                grpcPort,
                env,
                A3.human(),
                3,
                9,
                3,
                false,
                res.mutable_marker());
            txs = res.transactions();
            if (!BEAST_EXPECT(txs.size() == 3))
                return;
            BEAST_EXPECT(checkTransaction(txs[0u], 9, 7));
            BEAST_EXPECT(checkTransaction(txs[1u], 8, 7));
            BEAST_EXPECT(checkTransaction(txs[2u], 7, 6));
            if (!BEAST_EXPECT(res.has_marker()))
                return;

            std::tie(res, status) = next(
                grpcPort,
                env,
                A3.human(),
                3,
                9,
                3,
                false,
                res.mutable_marker());
            txs = res.transactions();
            if (!BEAST_EXPECT(txs.size() == 3))
                return;
            BEAST_EXPECT(checkTransaction(txs[0u], 6, 6));
            BEAST_EXPECT(checkTransaction(txs[1u], 5, 5));
            BEAST_EXPECT(checkTransaction(txs[2u], 4, 5));
            if (!BEAST_EXPECT(res.has_marker()))
                return;

            std::tie(res, status) = next(
                grpcPort,
                env,
                A3.human(),
                3,
                9,
                3,
                false,
                res.mutable_marker());
            txs = res.transactions();
            if (!BEAST_EXPECT(txs.size() == 3))
                return;
            BEAST_EXPECT(checkTransaction(txs[0u], 4, 4));
            BEAST_EXPECT(checkTransaction(txs[1u], 4, 4));
            BEAST_EXPECT(checkTransaction(txs[2u], 3, 3));
            if (!BEAST_EXPECT(res.has_marker()))
                return;

            std::tie(res, status) = next(
                grpcPort,
                env,
                A3.human(),
                3,
                9,
                3,
                false,
                res.mutable_marker());
            txs = res.transactions();
            if (!BEAST_EXPECT(txs.size() == 1))
                return;
            BEAST_EXPECT(checkTransaction(txs[0u], 3, 3));
            BEAST_EXPECT(!res.has_marker());
        }
    }

public:
    void
    run() override
    {
        testAccountTxPaging();
        testAccountTxPagingGrpc();
        testAccountTxParametersGrpc();
        testAccountTxContentsGrpc();
    }
};

BEAST_DEFINE_TESTSUITE(AccountTxPaging,app,ripple);

}

