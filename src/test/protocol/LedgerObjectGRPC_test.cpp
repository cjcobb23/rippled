//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2020 Ripple Labs Inc.

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

#include <ripple/basics/safe_cast.h>
#include <ripple/beast/unit_test.h>
#include <ripple/protocol/LedgerFormats.h>

#include "org/xrpl/rpc/v1/ledger_objects.pb.h"

#include <set>
#include <string>


namespace ripple {

class LedgerObjectGRPC_test : public beast::unit_test::suite
{
public:
    void testLedgerObjectGRPC ()
    {
        testcase ("Ledger object validation");

        // Create a namespace alias for shorter names.
        namespace grpc = org::xrpl::rpc::v1;

        // Verify all LedgerItems are handled by gRPC.
        //
        // At the moment the best validation we've been able to come up with
        // is comparing the names of Items in LedgerFormats to the names of
        // grpc::LedgerEntryTypes.  The names use different formatting so
        // some hackery is required.
        auto formatNameToEntryTypeName = [] (std::string fmtName) -> std::string
        {
            std::string entryName ("LEDGER_ENTRY_TYPE");
            for (char ch : fmtName)
            {
                if (std::isupper(ch))
                    entryName.push_back ('_');

                entryName.push_back (std::toupper(ch));
            }
            return entryName;
        };

        // Get corresponding LedgerEntryType names for all LedgerFormat Items.
        std::set<std::string> ledgerEntryTypeNames;
        LedgerFormats const& ledgerItems = LedgerFormats::getInstance();
        for (auto const& item : ledgerItems)
        {
            // grpc does not currently support Tickets.
            if (item.getType() == ltTICKET)
                continue;

            ledgerEntryTypeNames.insert (
                formatNameToEntryTypeName (item.getName()));
        }

        // For all ledger entry types, verify that they have a name
        // corresponding to the name of a LedgerFormats Item.
        std::size_t itemCount = 0u;
        using entryEnumType = std::underlying_type_t<grpc::LedgerEntryType>;
        for (entryEnumType i = grpc::LedgerEntryType_MIN;
            i <= grpc::LedgerEntryType_MAX; i += 1)
        {
            if (grpc::LedgerEntryType_IsValid (i))
            {
                grpc::LedgerEntryType e = safe_cast<grpc::LedgerEntryType>(i);

                // The UNSPECIFIED value will not be in LedgerFormats.
                if (e == grpc::LEDGER_ENTRY_TYPE_UNSPECIFIED)
                    continue;

                // Count LedgerEntryType and verify it has an expected string.
                itemCount += 1;
                std::string const& entryTypeName =
                    grpc::LedgerEntryType_Name(e);
                BEAST_EXPECT (ledgerEntryTypeNames.count(entryTypeName) == 1);
            }
        }

        // Verify that LedgerFormats and gRPC have the same number of entries.
        BEAST_EXPECT (itemCount == ledgerEntryTypeNames.size());
    }

    void run() override
    {
        testLedgerObjectGRPC();
    }
};

BEAST_DEFINE_TESTSUITE(LedgerObjectGRPC,protocol,ripple);

} // ripple
