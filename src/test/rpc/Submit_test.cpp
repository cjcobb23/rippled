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

#include <ripple/protocol/Feature.h>
#include <ripple/protocol/jss.h>
#include <test/jtx.h>

#include <ripple/rpc/GRPCHandlers.h>
#include <ripple/resource/Charge.h>
#include <ripple/resource/Fees.h>
#include <test/rpc/GRPCTestClientBase.h>

namespace ripple {
namespace test {

class Submit_test : public beast::unit_test::suite
{
public:

    //gRPC stuff
    class SubmitClient : public GRPCTestClientBase
    {
    public:
        rpc::v1::SubmitTransactionRequest request;
        rpc::v1::SubmitTransactionResponse reply;

        void SubmitTransaction()
        {
            status = stub_->SubmitTransaction(&context, request, &reply);
        }
    };

    struct TestData{
        std::string xrp_tx_blob;
        std::string xrp_tx_hash;
        std::string usd_tx_blob;
        std::string usd_tx_hash;
        const static int fund = 10000;
    } testData;

    void fillTestData()
    {
        testcase ("fill test data");

        using namespace jtx;
        Env env(*this);
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(TestData::fund), "alice", "bob");
        env.trust(bob["USD"](TestData::fund), alice);
        env.close();

        // use a websocket client to fill transaction blobs
        auto wsc = makeWSClient(env.app().config());
        {
            Json::Value jrequest_xrp;
            jrequest_xrp[jss::secret] = toBase58(generateSeed("alice"));
            jrequest_xrp[jss::tx_json] = pay("alice", "bob", XRP(TestData::fund/2));
            Json::Value jreply_xrp = wsc->invoke("sign", jrequest_xrp);

            if( !BEAST_EXPECT(jreply_xrp.isMember(jss::result)))
                return;
            if( !BEAST_EXPECT(jreply_xrp[jss::result].isMember(jss::tx_blob)))
                return;
            testData.xrp_tx_blob = textBlobToActualBlob(
                    jreply_xrp[jss::result][jss::tx_blob].asString());
            if( !BEAST_EXPECT(jreply_xrp[jss::result].isMember(jss::tx_json)))
                return;
            if( !BEAST_EXPECT(jreply_xrp[jss::result][jss::tx_json].isMember(jss::hash)))
                return;
            testData.xrp_tx_hash = textBlobToActualBlob(
                    jreply_xrp[jss::result][jss::tx_json][jss::hash].asString());
        }
        {
            Json::Value jrequest_usd;
            jrequest_usd[jss::secret] = toBase58(generateSeed("bob"));
            jrequest_usd[jss::tx_json] = pay("bob", "alice", bob["USD"](TestData::fund/2));
            Json::Value jreply_usd = wsc->invoke("sign", jrequest_usd);

            if (!BEAST_EXPECT(jreply_usd.isMember(jss::result)))
                return;
            if (!BEAST_EXPECT(jreply_usd[jss::result].isMember(jss::tx_blob)))
                return;
            testData.usd_tx_blob = textBlobToActualBlob(
                    jreply_usd[jss::result][jss::tx_blob].asString());
            if (!BEAST_EXPECT(jreply_usd[jss::result].isMember(jss::tx_json)))
                return;
            if (!BEAST_EXPECT(jreply_usd[jss::result][jss::tx_json].isMember(jss::hash)))
                return;
            testData.usd_tx_hash = textBlobToActualBlob(
                    jreply_usd[jss::result][jss::tx_json][jss::hash].asString());
        }
    }

    void testSubmitGoodBlobGrpc()
    {
        testcase ("Submit good blobs, XRP, USD, and same transaction twice");

        using namespace jtx;
        Env env(*this);
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(TestData::fund), "alice", "bob");
        env.trust(bob["USD"](TestData::fund), alice);
        env.close();

        //XRP
        {
            SubmitClient client;
            client.request.set_signed_transaction(testData.xrp_tx_blob);
            client.SubmitTransaction();
            if( !BEAST_EXPECT(client.status.ok()))
            {
                return;
            }
            BEAST_EXPECT(client.reply.engine_result() == "tesSUCCESS");
            BEAST_EXPECT(client.reply.engine_result_code() == 0);
            BEAST_EXPECT(client.reply.hash() == testData.xrp_tx_hash);
        }
        //USD
        {
            SubmitClient client;
            client.request.set_signed_transaction(testData.usd_tx_blob);
            client.SubmitTransaction();
            if( !BEAST_EXPECT(client.status.ok()))
            {
                return;
            }
            BEAST_EXPECT(client.reply.engine_result() == "tesSUCCESS");
            BEAST_EXPECT(client.reply.engine_result_code() == 0);
            BEAST_EXPECT(client.reply.hash() == testData.usd_tx_hash);
        }
        //USD, error, same transaction again
        {
            SubmitClient client;
            client.request.set_signed_transaction(testData.usd_tx_blob);
            client.SubmitTransaction();
            if( !BEAST_EXPECT(client.status.ok()))
            {
                return;
            }
            BEAST_EXPECT(client.reply.engine_result() == "tefALREADY");
            BEAST_EXPECT(client.reply.engine_result_code() == -198);
        }
    }

    void testSubmitErrorBlobGrpc()
    {
        testcase ("Submit error, bad blob, no account");

        using namespace jtx;
        Env env(*this);

        //short transaction blob, cannot parse
        {
            SubmitClient client;
            client.request.set_signed_transaction("deadbeef");
            client.SubmitTransaction();
            BEAST_EXPECT( !client.status.ok());
        }
        //bad blob with correct length, cannot parse
        {
            SubmitClient client;
            auto xrp_tx_blob_copy(testData.xrp_tx_blob);
            std::reverse(xrp_tx_blob_copy.begin(), xrp_tx_blob_copy.end());
            client.request.set_signed_transaction(xrp_tx_blob_copy);
            client.SubmitTransaction();
            BEAST_EXPECT( !client.status.ok());
        }
        //good blob, can parse but no account
        {
            SubmitClient client;
            client.request.set_signed_transaction(testData.xrp_tx_blob);
            client.SubmitTransaction();
            if( !BEAST_EXPECT(client.status.ok()))
            {
                return;
            }
            BEAST_EXPECT(client.reply.engine_result() == "terNO_ACCOUNT");
            BEAST_EXPECT(client.reply.engine_result_code() == -96);
        }
    }

    void testSubmitInsufficientFundsGrpc()
    {
        testcase("Submit good blobs but insufficient funds");

        using namespace jtx;
        Env env(*this);
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        // fund 1000 (TestData::fund/10) XRP, the transaction sends 5000 (TestData::fund/2) XRP,
        // so insufficient funds
        env.fund(XRP(TestData::fund/10), "alice", "bob");
        env.trust(bob["USD"](TestData::fund), alice);
        env.close();

        {
            SubmitClient client;
            client.request.set_signed_transaction(testData.xrp_tx_blob);
            client.SubmitTransaction();
            if( !BEAST_EXPECT(client.status.ok()))
            {
                return;
            }
            BEAST_EXPECT(client.reply.engine_result() == "tecUNFUNDED_PAYMENT");
            BEAST_EXPECT(client.reply.engine_result_code() == 104);
        }
    }

    void run() override
    {
        fillTestData();
        testSubmitGoodBlobGrpc();
        testSubmitErrorBlobGrpc();
        testSubmitInsufficientFundsGrpc();
    }
};

BEAST_DEFINE_TESTSUITE(Submit,app,ripple);

}
}
