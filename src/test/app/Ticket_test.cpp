//------------------------------------------------------------------------------
/*
  This file is part of rippled: https://github.com/ripple/rippled
  Copyright (c) 2012-2016 Ripple Labs Inc.

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
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/jss.h>

namespace ripple {

class Ticket_test : public beast::unit_test::suite
{
    /// @brief Validate metadata for a successful CreateTicket transaction.
    ///
    /// @param env current jtx env (tx and meta are extracted using it)
    void checkTicketCreateMeta (test::jtx::Env& env)
    {
        Json::Value const& tx {env.tx()->getJson (JsonOptions::none)};
        {
            std::string const txType = tx[sfTransactionType.jsonName].asString();
            if (txType != jss::TicketCreate)
            {
                fail (std::string {"Unexpected TransactionType: "} + txType,
                    __FILE__, __LINE__);
                return;
            }
        }
        std::uint32_t const count = {tx[sfCount.jsonName].asUInt()};
        if (count < 1)
        {
            fail (std::string {"Unexpected ticket count: "} +
                std::to_string (count), __FILE__, __LINE__);
            return;
        }
        std::uint32_t const txSeq = {tx[sfSequence.jsonName].asUInt()};
        std::string const account = tx[sfAccount.jsonName].asString();

        Json::Value const& metadata = env.meta()->getJson (JsonOptions::none);
        if (! metadata.isMember (sfTransactionResult.fieldName) ||
            metadata[sfTransactionResult.jsonName].asString() != "tesSUCCESS")
        {
            fail ("Not metadata for successful TicketCreate.",
                __FILE__, __LINE__);
            return;
        }

        BEAST_EXPECT (metadata.isMember (sfAffectedNodes.fieldName));
        BEAST_EXPECT (metadata[sfAffectedNodes.fieldName].isArray());

        bool directoryChanged = false;
        std::uint32_t acctRootFinalSeq = {0};
        std::vector<std::uint32_t> ticketSeqs;
        ticketSeqs.reserve (count);
        for (Json::Value const& node : metadata[sfAffectedNodes.fieldName])
        {
            if (node.isMember (sfModifiedNode.jsonName))
            {
                Json::Value const modified = node[sfModifiedNode.jsonName];
                std::string const entryType =
                    modified[sfLedgerEntryType.jsonName].asString();
                if (entryType == jss::AccountRoot)
                {
                    {
                        // Verify the account root Sequence did the right thing.
                        std::uint32_t const prevSeq = {
                            modified[sfPreviousFields.jsonName]
                                [sfSequence.jsonName].asUInt()};

                        acctRootFinalSeq =
                            modified[sfFinalFields.jsonName]
                                [sfSequence.jsonName].asUInt();

                        if (txSeq == 0)
                        {
                            // Transaction used a TicketSequence.
                            BEAST_EXPECT (acctRootFinalSeq == prevSeq + count);
                        }
                        else
                        {
                            // Transaction used a (plain) Sequence.
                            BEAST_EXPECT (prevSeq == txSeq);
                            BEAST_EXPECT (
                                acctRootFinalSeq == prevSeq + count + 1);
                        }
                    }

                    std::uint32_t const consumedTickets =
                        {txSeq == 0u ? 1u : 0u};

                    // If...
                    //  1. The TicketCount is 1 and
                    //  2. A ticket was consumed by the ticket create, then
                    //  3. The final TicketCount did not change, so the
                    //     previous TicketCount is not reported.
                    // But, since the count did not change, we know it equals
                    // the final Ticket count.
                    bool const unreportedPrevTicketCount =
                        {count == 1 && txSeq == 0};

                    // Verify the OwnerCount did the right thing
                    if (unreportedPrevTicketCount)
                    {
                        // The number of Tickets should not have changed, so
                        // the previous OwnerCount should not be reported.
                        BEAST_EXPECT (!
                            modified[sfPreviousFields.jsonName].isMember(
                                sfOwnerCount.jsonName));
                    }
                    else
                    {
                        // Verify the OwnerCount did the right thing.
                        std::uint32_t const prevCount = {
                            modified[sfPreviousFields.jsonName]
                                [sfOwnerCount.jsonName].asUInt()};

                        std::uint32_t const finalCount = {
                            modified[sfFinalFields.jsonName]
                                [sfOwnerCount.jsonName].asUInt()};

                        BEAST_EXPECT (
                            prevCount + count - consumedTickets == finalCount);
                    }

                    // Verify TicketCount metadata.
                    if (unreportedPrevTicketCount)
                    {
                        // The number of Tickets should not have changed, so
                        // the previous TicketCount should not be reported.
                        BEAST_EXPECT (!
                            modified[sfPreviousFields.jsonName].isMember(
                                sfTicketCount.jsonName));
                    }
                    else
                    {
                        // If the TicketCount was previously present it
                        // should have been greater than zero.
                        std::uint32_t const startCount = {
                            modified[sfPreviousFields.jsonName].isMember(
                                sfTicketCount.jsonName) ?
                            modified[sfPreviousFields.jsonName]
                                [sfTicketCount.jsonName].asUInt() : 0u};

                        if (startCount == 0u)
                            BEAST_EXPECT (!
                                modified[sfPreviousFields.jsonName].isMember(
                                    sfTicketCount.jsonName));

                        BEAST_EXPECT (
                            startCount + count - consumedTickets ==
                                modified[sfFinalFields.jsonName]
                                    [sfTicketCount.jsonName]);
                    }
                }
                else if (entryType == jss::DirectoryNode)
                {
                    directoryChanged = true;
                }
                else
                {
                    fail (
                        std::string {"Unexpected modified node: "} + entryType,
                        __FILE__, __LINE__);
                }
            }
            else if (node.isMember (sfCreatedNode.jsonName))
            {
                Json::Value const created = node[sfCreatedNode.jsonName];
                std::string const entryType =
                    created[sfLedgerEntryType.jsonName].asString();
                if (entryType == jss::Ticket)
                {
                    BEAST_EXPECT (
                        created[sfNewFields.jsonName]
                            [sfAccount.jsonName].asString() == account);
                    ticketSeqs.push_back (
                        created[sfNewFields.jsonName]
                            [sfTicketSequence.jsonName].asUInt());
                }
                else if (entryType == jss::DirectoryNode)
                {
                    directoryChanged = true;
                }
                else
                {
                    fail (
                        std::string {"Unexpected created node: "} + entryType,
                        __FILE__, __LINE__);
                }
            }
            else if (node.isMember (sfDeletedNode.jsonName))
            {
                Json::Value const deleted = node[sfDeletedNode.jsonName];
                std::string const entryType =
                    deleted[sfLedgerEntryType.jsonName].asString();

                if (entryType == jss::Ticket)
                {
                    // Verify the transaction's Sequence == 0.
                    BEAST_EXPECT (txSeq == 0);

                    // Verify the account of the deleted ticket.
                    BEAST_EXPECT (deleted[sfFinalFields.jsonName]
                        [sfAccount.jsonName].asString() == account);

                    // Verify the deleted ticket has the right TicketSequence.
                    BEAST_EXPECT (deleted[sfFinalFields.jsonName]
                        [sfTicketSequence.jsonName].asUInt() ==
                            tx[sfTicketSequence.jsonName].asUInt());
                }
            }
            else
            {
                fail ("Unexpected node type in TicketCreate metadata.",
                    __FILE__, __LINE__);
            }
        }
        BEAST_EXPECT (directoryChanged == true);

        // Verify that all the expected Tickets were created.
        BEAST_EXPECT (ticketSeqs.size() == count);
        std::sort (ticketSeqs.begin(), ticketSeqs.end());
        BEAST_EXPECT (std::adjacent_find (
            ticketSeqs.begin(), ticketSeqs.end()) == ticketSeqs.end());
        BEAST_EXPECT (*ticketSeqs.rbegin() == acctRootFinalSeq - 1);
    }

    void testTicketNotEnabled ()
    {
        testcase ("Feature Not Enabled");

        using namespace test::jtx;
        Env env {*this, supported_amendments() - featureTicketBatch};

        env (ticket::create (env.master, 1), ter(temDISABLED));
    }

    void testTicketCreatePreflightFail ()
    {
        testcase ("Create Tickets that fail Preflight");

        using namespace test::jtx;
        Env env {*this};

        Account const master {env.master};

        // Exercise fees.
        env (ticket::create (master, 1), fee (XRP (10)));
        checkTicketCreateMeta (env);
        env.close();
        env.require(owners (master, 1), tickets (master, 1));

        env (ticket::create (master, 1), fee (XRP (-1)), ter (temBAD_FEE));

        // Exercise flags.
        env (ticket::create (master, 1), txflags (tfFullyCanonicalSig));
        checkTicketCreateMeta (env);
        env.close();
        env.require(owners (master, 2), tickets (master, 2));

        env (ticket::create (master, 1),
            txflags (tfSell), ter (temINVALID_FLAG));

        // Exercise boundaries on count.
        env (ticket::create (master, 0), ter (temINVALID_COUNT));
        env (ticket::create (master, 251), ter (temINVALID_COUNT));
        env.close();
        env.require (owners (master, 2), tickets (master, 2));

        env (ticket::create (master, 248));
        checkTicketCreateMeta (env);
        env.close();
        env.require (owners (master, 250), tickets (master, 250));
    }

    void testTicketCreatePreclaimFail ()
    {
        testcase ("Create Tickets that fail Preclaim");

        using namespace test::jtx;
        {
            // Create tickets on a non-existent account.
            Env env {*this};
            Account alice {"alice"};
            env.memoize (alice);

            env (ticket::create (alice, 1),
                json (jss::Sequence, 1),
                ter (terNO_ACCOUNT));
        }
        {
            // Exceed the threshold where tickets can no longer be
            // added to an account.
            Env env {*this};
            Account alice {"alice"};

            env.fund (XRP (100000), alice);
            env (ticket::create (alice, 250));
            checkTicketCreateMeta (env);
            env (ticket::create (alice, 1), ter (tecDIR_FULL));
            env.close();
            env.require (owners (alice, 250), tickets (alice, 250));
        }
        {
            // Another way to exceed the threshold where tickets can no longer
            // beadded to an account.
            Env env {*this};
            Account alice {"alice"};

            env.fund (XRP (100000), alice);
            env (ticket::create (alice, 1));
            checkTicketCreateMeta (env);
            env (ticket::create (alice, 250), ter (tecDIR_FULL));
            env.close();
            env.require (owners (alice, 1), tickets (alice, 1));
        }
    }

    void testTicketInsufficientReserve ()
    {
        testcase ("Create Ticket Insufficient Reserve");

        using namespace test::jtx;
        Env env {*this};
        Account alice {"alice"};

        // Fund alice not quite enough to make the reserve for a Ticket.
        env.fund (env.current()->fees().accountReserve (1) - drops (1), alice);
        env.close();
        env.require (balance (alice,
            env.current()->fees().accountReserve (1) - drops (1)));

        env (ticket::create (alice, 1), ter (tecINSUFFICIENT_RESERVE));
        env.close();
        env.require (owners (alice, 0), tickets (alice, 0));

        // Give alice enough to exactly meet the reserve for one Ticket.
        XRPAmount const fee {env.current()->fees().base};
        env (pay (env.master, alice, fee + (drops (1))));
        env.close();
        env.require (balance (alice, env.current()->fees().accountReserve (1)));

        env (ticket::create (alice, 1));
        checkTicketCreateMeta (env);
        env.close();
        env.require (owners (alice, 1), tickets (alice, 1));

        // Give alice not quite enough to make the reserve for a total of
        // 250 Tickets.
        env (pay (env.master, alice,
            env.current()->fees().accountReserve (250) - drops (1) -
            env.balance (alice)));
        env.close();
        env.require (balance (
            alice, env.current()->fees().accountReserve (250) - drops (1)));

        // alice doesn't quite have the reserve for a total of 250
        // Tickets, so the transaction fails.
        env (ticket::create (alice, 249), ter (tecINSUFFICIENT_RESERVE));
        env.close();
        env.require (owners (alice, 1), tickets (alice, 1));

        // Give alice enough so she can make the reserve for all 250
        // Tickets.
        env (pay (env.master, alice,
            env.current()->fees().accountReserve (250) - env.balance (alice)));
        env.close();

        std::uint32_t const ticketSeq {env.seq (alice) + 1};
        env (ticket::create (alice, 249));
        checkTicketCreateMeta (env);
        env.close();
        env.require (owners (alice, 250), tickets (alice, 250));
        BEAST_EXPECT (ticketSeq + 249 == env.seq (alice));
    }

public:
    void run () override
    {
        testTicketNotEnabled();
        testTicketCreatePreflightFail();
        testTicketCreatePreclaimFail();
        testTicketInsufficientReserve();
    }
};

BEAST_DEFINE_TESTSUITE (Ticket, tx, ripple);

}  // ripple

