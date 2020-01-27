//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2014 Ripple Labs Inc.

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
#include <ripple/app/ledger/OpenLedger.h>
#include <ripple/app/misc/Transaction.h>
#include <ripple/ledger/View.h>
#include <ripple/net/RPCErr.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/Feature.h>
#include <ripple/rpc/Context.h>
#include <ripple/rpc/DeliveredAmount.h>
#include <ripple/rpc/impl/RPCHelpers.h>
#include <boost/algorithm/string/case_conv.hpp>

#include <ripple/rpc/impl/GRPCHelpers.h>

namespace ripple {
namespace RPC {

boost::optional<AccountID>
accountFromStringStrict(std::string const& account)
{
    boost::optional <AccountID> result;

    auto const publicKey = parseBase58<PublicKey> (
        TokenType::AccountPublic,
        account);

    if (publicKey)
        result = calcAccountID (*publicKey);
    else
        result = parseBase58<AccountID> (account);

    return result;
}

error_code_i
accountFromStringWithCode(
    AccountID& result, std::string const& strIdent, bool bStrict)
{
    if (auto accountID = accountFromStringStrict (strIdent))
    {
        result = *accountID;
        return rpcSUCCESS;
    }

    if (bStrict)
    {
        auto id = deprecatedParseBitcoinAccountID (strIdent);
        return id ? rpcACT_BITCOIN : rpcACT_MALFORMED;
    }

    // We allow the use of the seeds which is poor practice
    // and merely for debugging convenience.
    auto const seed = parseGenericSeed (strIdent);

    if (!seed)
        return rpcBAD_SEED;

    auto const keypair = generateKeyPair (
        KeyType::secp256k1,
        *seed);

    result = calcAccountID (keypair.first);
    return rpcSUCCESS;
}

Json::Value
accountFromString(
    AccountID& result, std::string const& strIdent, bool bStrict)
{

    error_code_i code = accountFromStringWithCode(result, strIdent, bStrict);
    if(code != rpcSUCCESS)
        return rpcError(code);
    else
        return Json::objectValue;
}

bool
getAccountObjects(ReadView const& ledger, AccountID const& account,
    boost::optional<std::vector<LedgerEntryType>> const& typeFilter, uint256 dirIndex,
    uint256 const& entryIndex, std::uint32_t const limit, Json::Value& jvResult)
{
    auto const rootDirIndex = getOwnerDirIndex (account);
    auto found = false;

    if (dirIndex.isZero ())
    {
        dirIndex = rootDirIndex;
        found = true;
    }

    auto dir = ledger.read({ltDIR_NODE, dirIndex});
    if (! dir)
        return false;

    std::uint32_t i = 0;
    auto& jvObjects = (jvResult[jss::account_objects] = Json::arrayValue);
    for (;;)
    {
        auto const& entries = dir->getFieldV256 (sfIndexes);
        auto iter = entries.begin ();

        if (! found)
        {
            iter = std::find (iter, entries.end (), entryIndex);
            if (iter == entries.end ())
                return false;

            found = true;
        }

        for (; iter != entries.end (); ++iter)
        {
            auto const sleNode = ledger.read(keylet::child(*iter));

            auto typeMatchesFilter = [] (
                std::vector<LedgerEntryType> const& typeFilter,
                LedgerEntryType ledgerType)
            {
                auto it = std::find(typeFilter.begin(), typeFilter.end(),
                    ledgerType);
                return it != typeFilter.end();
            };

            if (!typeFilter.has_value() ||
                typeMatchesFilter(typeFilter.value(), sleNode->getType()))
            {
                jvObjects.append (sleNode->getJson (JsonOptions::none));

                if (++i == limit)
                {
                    if (++iter != entries.end ())
                    {
                        jvResult[jss::limit] = limit;
                        jvResult[jss::marker] = to_string (dirIndex) + ',' +
                            to_string (*iter);
                        return true;
                    }

                    break;
                }
            }
        }

        auto const nodeIndex = dir->getFieldU64 (sfIndexNext);
        if (nodeIndex == 0)
            return true;

        dirIndex = getDirNodeIndex (rootDirIndex, nodeIndex);
        dir = ledger.read({ltDIR_NODE, dirIndex});
        if (! dir)
            return true;

        if (i == limit)
        {
            auto const& e = dir->getFieldV256 (sfIndexes);
            if (! e.empty ())
            {
                jvResult[jss::limit] = limit;
                jvResult[jss::marker] = to_string (dirIndex) + ',' +
                    to_string (*e.begin ());
            }

            return true;
        }
    }
}

namespace {

bool
isValidatedOld(LedgerMaster& ledgerMaster, bool standalone)
{
    if (standalone)
        return false;

    return ledgerMaster.getValidatedLedgerAge () >
        Tuning::maxValidatedLedgerAge;
}

template <class T>
Status
ledgerFromRequest(T& ledger, JsonContext& context)
{
    static auto const minSequenceGap = 10;

    ledger.reset();

    auto& params = context.params;
    auto& ledgerMaster = context.ledgerMaster;

    auto indexValue = params[jss::ledger_index];
    auto hashValue = params[jss::ledger_hash];

    // We need to support the legacy "ledger" field.
    auto& legacyLedger = params[jss::ledger];
    if (legacyLedger)
    {
        if (legacyLedger.asString().size () > 12)
            hashValue = legacyLedger;
        else
            indexValue = legacyLedger;
    }

    if (hashValue)
    {
        if (! hashValue.isString ())
            return {rpcINVALID_PARAMS, "ledgerHashNotString"};

        uint256 ledgerHash;
        if (! ledgerHash.SetHex (hashValue.asString ()))
            return {rpcINVALID_PARAMS, "ledgerHashMalformed"};

        ledger = ledgerMaster.getLedgerByHash (ledgerHash);
        if (ledger == nullptr)
            return {rpcLGR_NOT_FOUND, "ledgerNotFound"};
    }
    else if (indexValue.isNumeric())
    {
        ledger = ledgerMaster.getLedgerBySeq (indexValue.asInt ());

        if (ledger == nullptr)
        {
            auto cur = ledgerMaster.getCurrentLedger();
            if (cur->info().seq == indexValue.asInt())
                ledger = cur;
        }

        if (ledger == nullptr)
            return {rpcLGR_NOT_FOUND, "ledgerNotFound"};

        if (ledger->info().seq > ledgerMaster.getValidLedgerIndex() &&
            isValidatedOld(ledgerMaster, context.app.config().standalone()))
        {
            ledger.reset();
            return {rpcNO_NETWORK, "InsufficientNetworkMode"};
        }
    }
    else
    {
        if (isValidatedOld (ledgerMaster, context.app.config().standalone()))
            return {rpcNO_NETWORK, "InsufficientNetworkMode"};

        auto const index = indexValue.asString ();
        if (index == "validated")
        {
            ledger = ledgerMaster.getValidatedLedger ();
            if (ledger == nullptr)
                return {rpcNO_NETWORK, "InsufficientNetworkMode"};

            assert (! ledger->open());
        }
        else
        {
            if (index.empty () || index == "current")
            {
                ledger = ledgerMaster.getCurrentLedger ();
                assert (ledger->open());
            }
            else if (index == "closed")
            {
                ledger = ledgerMaster.getClosedLedger ();
                assert (! ledger->open());
            }
            else
            {
                return {rpcINVALID_PARAMS, "ledgerIndexMalformed"};
            }

            if (ledger == nullptr)
                return {rpcNO_NETWORK, "InsufficientNetworkMode"};

            if (ledger->info().seq + minSequenceGap <
                ledgerMaster.getValidLedgerIndex ())
            {
                ledger.reset ();
                return {rpcNO_NETWORK, "InsufficientNetworkMode"};
            }
        }
    }

    return Status::OK;
}
}  // namespace

template <class T>
Status
ledgerFromRequest(
    T& ledger,
    GRPCContext<rpc::v1::GetAccountInfoRequest>& context)
{
    static auto const minSequenceGap = 10;

    ledger.reset();

    rpc::v1::GetAccountInfoRequest& request = context.params;
    auto& ledgerMaster = context.ledgerMaster;

    using LedgerCase = rpc::v1::LedgerSpecifier::LedgerCase;
    LedgerCase ledgerCase = request.ledger().ledger_case();


    std::cout << ledgerCase << std::endl;

    if (ledgerCase == LedgerCase::kHash)
    {
        std::cout << "hash" << std::endl;
        uint256 ledgerHash = uint256::fromVoid(request.ledger().hash().data());
        if (ledgerHash.size() != request.ledger().hash().size())
            return {rpcINVALID_PARAMS, "ledgerHashMalformed"};

        ledger = ledgerMaster.getLedgerByHash(ledgerHash);
        if (ledger == nullptr)
            return {rpcLGR_NOT_FOUND, "ledgerNotFound"};
    }
    else if (ledgerCase == LedgerCase::kSequence)
    {

        std::cout << "sequence" << std::endl;
        ledger = ledgerMaster.getLedgerBySeq(request.ledger().sequence());

        if (ledger == nullptr)
        {
            auto cur = ledgerMaster.getCurrentLedger();
            if (cur->info().seq == request.ledger().sequence())
                ledger = cur;
        }

        if (ledger == nullptr)
            return {rpcLGR_NOT_FOUND, "ledgerNotFound"};

        if (ledger->info().seq > ledgerMaster.getValidLedgerIndex() &&
            isValidatedOld(ledgerMaster, context.app.config().standalone()))
        {
            ledger.reset();
            return {rpcNO_NETWORK, "InsufficientNetworkMode"};
        }
    }
    else if (
        ledgerCase == LedgerCase::kShortcut ||
        ledgerCase == LedgerCase::LEDGER_NOT_SET)
    {
        if(ledgerCase == LedgerCase::kShortcut)
        {
            std::cout << "shortcut" << std::endl;
        }
        else
        {
            std::cout << "not set" << std::endl;
        }
        if (isValidatedOld(ledgerMaster, context.app.config().standalone()))
            return {rpcNO_NETWORK, "InsufficientNetworkMode"};

        auto const shortcut = request.ledger().shortcut();
        if (shortcut == rpc::v1::LedgerSpecifier::SHORTCUT_VALIDATED)
        {
            ledger = ledgerMaster.getValidatedLedger();
            if (ledger == nullptr)
                return {rpcNO_NETWORK, "InsufficientNetworkMode"};

            assert(!ledger->open());
        }
        else
        {
            // note, if unspecified, defaults to current ledger
            if (shortcut == rpc::v1::LedgerSpecifier::SHORTCUT_UNSPECIFIED ||
                shortcut == rpc::v1::LedgerSpecifier::SHORTCUT_CURRENT)
            {
                ledger = ledgerMaster.getCurrentLedger();
                assert(ledger->open());
            }
            else if (shortcut == rpc::v1::LedgerSpecifier::SHORTCUT_CLOSED)
            {
                ledger = ledgerMaster.getClosedLedger();
                assert(!ledger->open());
            }

            if (ledger == nullptr)
                return {rpcNO_NETWORK, "InsufficientNetworkMode"};

            if (ledger->info().seq + minSequenceGap <
                ledgerMaster.getValidLedgerIndex())
            {
                ledger.reset();
                return {rpcNO_NETWORK, "InsufficientNetworkMode"};
            }
        }
    }

    return Status::OK;
}

// explicit instantiation of above function
template Status
ledgerFromRequest<>(
    std::shared_ptr<ReadView const>&,
    GRPCContext<rpc::v1::GetAccountInfoRequest>&);

template <class T>
Status
getLedger(T& ledger, uint256 const & ledgerHash, Context& context)
{
        ledger = context.ledgerMaster.getLedgerByHash(ledgerHash);
        if (ledger == nullptr)
            return {rpcLGR_NOT_FOUND, "ledgerNotFound"};
        return Status::OK;
}

template <class T>
Status
getLedger(T& ledger, uint32_t ledgerIndex, Context& context)
{
    ledger = context.ledgerMaster.getLedgerBySeq(ledgerIndex);
    if (ledger == nullptr)
    {
        auto cur = context.ledgerMaster.getCurrentLedger();
        if (cur->info().seq == ledgerIndex)
        {
            ledger = cur;
        }
    }

    if (ledger == nullptr)
        return {rpcLGR_NOT_FOUND, "ledgerNotFound"};

    if (ledger->info().seq > context.ledgerMaster.getValidLedgerIndex() &&
            isValidatedOld(context.ledgerMaster, context.app.config().standalone()))
    {
        ledger.reset();
        return {rpcNO_NETWORK, "InsufficientNetworkMode"};
    }

    return Status::OK;
}


template <class T>
Status
getLedger(T& ledger, LedgerShortcut shortcut, Context& context)
{
    if (isValidatedOld (context.ledgerMaster, context.app.config().standalone()))
        return {rpcNO_NETWORK, "InsufficientNetworkMode"};

    if (shortcut == LedgerShortcut::VALIDATED)
    {
        ledger = context.ledgerMaster.getValidatedLedger ();
        if (ledger == nullptr)
            return {rpcNO_NETWORK, "InsufficientNetworkMode"};

        assert (! ledger->open());
    }
    else
    {
        if (shortcut == LedgerShortcut::CURRENT)
        {
            ledger = context.ledgerMaster.getCurrentLedger ();
            assert (ledger->open());
        }
        else if (shortcut == LedgerShortcut::CLOSED)
        {
            ledger = context.ledgerMaster.getClosedLedger ();
            assert (! ledger->open());
        }
        else
        {
            return {rpcINVALID_PARAMS, "ledgerIndexMalformed"};
        }

        if (ledger == nullptr)
            return {rpcNO_NETWORK, "InsufficientNetworkMode"};

        static auto const minSequenceGap = 10;

        if (ledger->info().seq + minSequenceGap <
                context.ledgerMaster.getValidLedgerIndex ())
        {
            ledger.reset ();
            return {rpcNO_NETWORK, "InsufficientNetworkMode"};
        }
    }
    return Status::OK;
}

//Explicit instantiaion of above three functions
template Status
getLedger<>(std::shared_ptr<ReadView const>&,
        uint256 const&, Context&);

template Status
getLedger<>(std::shared_ptr<ReadView const>&,
        uint32_t, Context&);

template Status
getLedger<>(std::shared_ptr<ReadView const>&,
        LedgerShortcut shortcut, Context&);

bool
isValidated(LedgerMaster& ledgerMaster, ReadView const& ledger,
    Application& app)
{
    if (ledger.open())
        return false;

    if (ledger.info().validated)
        return true;

    auto seq = ledger.info().seq;
    try
    {
        // Use the skip list in the last validated ledger to see if ledger
        // comes before the last validated ledger (and thus has been
        // validated).
        auto hash = ledgerMaster.walkHashBySeq (seq);

        if (!hash || ledger.info().hash != *hash)
        {
            // This ledger's hash is not the hash of the validated ledger
            if (hash)
            {
                assert(hash->isNonZero());
                uint256 valHash = getHashByIndex (seq, app);
                if (valHash == ledger.info().hash)
                {
                    // SQL database doesn't match ledger chain
                    ledgerMaster.clearLedger (seq);
                }
            }
            return false;
        }
    }
    catch (SHAMapMissingNode const&)
    {
        auto stream = app.journal ("RPCHandler").warn();
        JLOG (stream)
            << "Missing SHANode " << std::to_string (seq);
        return false;
    }

    // Mark ledger as validated to save time if we see it again.
    ledger.info().validated = true;
    return true;
}


// The previous version of the lookupLedger command would accept the
// "ledger_index" argument as a string and silently treat it as a request to
// return the current ledger which, while not strictly wrong, could cause a lot
// of confusion.
//
// The code now robustly validates the input and ensures that the only possible
// values for the "ledger_index" parameter are the index of a ledger passed as
// an integer or one of the strings "current", "closed" or "validated".
// Additionally, the code ensures that the value passed in "ledger_hash" is a
// string and a valid hash. Invalid values will return an appropriate error
// code.
//
// In the absence of the "ledger_hash" or "ledger_index" parameters, the code
// assumes that "ledger_index" has the value "current".
//
// Returns a Json::objectValue.  If there was an error, it will be in that
// return value.  Otherwise, the object contains the field "validated" and
// optionally the fields "ledger_hash", "ledger_index" and
// "ledger_current_index", if they are defined.
Status
lookupLedger(std::shared_ptr<ReadView const>& ledger, JsonContext& context,
    Json::Value& result)
{
    if (auto status = ledgerFromRequest (ledger, context))
        return status;

    auto& info = ledger->info();

    if (!ledger->open())
    {
        result[jss::ledger_hash] = to_string (info.hash);
        result[jss::ledger_index] = info.seq;
    }
    else
    {
        result[jss::ledger_current_index] = info.seq;
    }

    result[jss::validated] = isValidated (context.ledgerMaster, *ledger, context.app);
    return Status::OK;
}

Json::Value
lookupLedger(std::shared_ptr<ReadView const>& ledger, JsonContext& context)
{
    Json::Value result;
    if (auto status = lookupLedger (ledger, context, result))
        status.inject (result);

    return result;
}

hash_set<AccountID>
parseAccountIds(Json::Value const& jvArray)
{
    hash_set<AccountID> result;
    for (auto const& jv: jvArray)
    {
        if (! jv.isString())
            return hash_set<AccountID>();
        auto const id =
            parseBase58<AccountID>(jv.asString());
        if (! id)
            return hash_set<AccountID>();
        result.insert(*id);
    }
    return result;
}

void
injectSLE(Json::Value& jv, SLE const& sle)
{
    jv = sle.getJson(JsonOptions::none);
    if (sle.getType() == ltACCOUNT_ROOT)
    {
        if (sle.isFieldPresent(sfEmailHash))
        {
            auto const& hash =
                sle.getFieldH128(sfEmailHash);
            Blob const b (hash.begin(), hash.end());
            std::string md5 = strHex(makeSlice(b));
            boost::to_lower(md5);
            // VFALCO TODO Give a name and move this constant
            //             to a more visible location. Also
            //             shouldn't this be https?
            jv[jss::urlgravatar] = str(boost::format(
                "http://www.gravatar.com/avatar/%s") % md5);
        }
    }
    else
    {
        jv[jss::Invalid] = true;
    }
}

boost::optional<Json::Value>
readLimitField(unsigned int& limit, Tuning::LimitRange const& range,
    JsonContext const& context)
{
    limit = range.rdefault;
    if (auto const& jvLimit = context.params[jss::limit])
    {
        if (! (jvLimit.isUInt() || (jvLimit.isInt() && jvLimit.asInt() >= 0)))
            return RPC::expected_field_error (jss::limit, "unsigned integer");

        limit = jvLimit.asUInt();
        if (! isUnlimited (context.role))
            limit = std::max(range.rmin, std::min(range.rmax, limit));
    }
    return boost::none;
}

boost::optional<Seed>
parseRippleLibSeed(Json::Value const& value)
{
    // ripple-lib encodes seed used to generate an Ed25519 wallet in a
    // non-standard way. While rippled never encode seeds that way, we
    // try to detect such keys to avoid user confusion.
    if (!value.isString())
        return boost::none;

    auto const result = decodeBase58Token(value.asString(), TokenType::None);

    if (result.size() == 18 &&
            static_cast<std::uint8_t>(result[0]) == std::uint8_t(0xE1) &&
            static_cast<std::uint8_t>(result[1]) == std::uint8_t(0x4B))
        return Seed(makeSlice(result.substr(2)));

    return boost::none;
}

boost::optional<Seed>
getSeedFromRPC(Json::Value const& params, Json::Value& error)
{
    // The array should be constexpr, but that makes Visual Studio unhappy.
    static char const* const seedTypes[]
    {
        jss::passphrase.c_str(),
        jss::seed.c_str(),
        jss::seed_hex.c_str()
    };

    // Identify which seed type is in use.
    char const* seedType = nullptr;
    int count = 0;
    for (auto t : seedTypes)
    {
        if (params.isMember (t))
        {
            ++count;
            seedType = t;
        }
    }

    if (count != 1)
    {
        error = RPC::make_param_error (
            "Exactly one of the following must be specified: " +
            std::string(jss::passphrase) + ", " +
            std::string(jss::seed) + " or " +
            std::string(jss::seed_hex));
        return boost::none;
    }

    // Make sure a string is present
    if (! params[seedType].isString())
    {
        error = RPC::expected_field_error (seedType, "string");
        return boost::none;
    }

    auto const fieldContents = params[seedType].asString();

    // Convert string to seed.
    boost::optional<Seed> seed;

    if (seedType == jss::seed.c_str())
        seed = parseBase58<Seed> (fieldContents);
    else if (seedType == jss::passphrase.c_str())
        seed = parseGenericSeed (fieldContents);
    else if (seedType == jss::seed_hex.c_str())
    {
        uint128 s;

        if (s.SetHexExact (fieldContents))
            seed.emplace (Slice(s.data(), s.size()));
    }

    if (!seed)
        error = rpcError (rpcBAD_SEED);

    return seed;
}

std::pair<PublicKey, SecretKey>
keypairForSignature(Json::Value const& params, Json::Value& error)
{
    bool const has_key_type  = params.isMember (jss::key_type);

    // All of the secret types we allow, but only one at a time.
    // The array should be constexpr, but that makes Visual Studio unhappy.
    static char const* const secretTypes[]
    {
        jss::passphrase.c_str(),
        jss::secret.c_str(),
        jss::seed.c_str(),
        jss::seed_hex.c_str()
    };

    // Identify which secret type is in use.
    char const* secretType = nullptr;
    int count = 0;
    for (auto t : secretTypes)
    {
        if (params.isMember (t))
        {
            ++count;
            secretType = t;
        }
    }

    if (count == 0 || secretType == nullptr)
    {
        error = RPC::missing_field_error (jss::secret);
        return { };
    }

    if (count > 1)
    {
        error = RPC::make_param_error (
            "Exactly one of the following must be specified: " +
            std::string(jss::passphrase) + ", " +
            std::string(jss::secret) + ", " +
            std::string(jss::seed) + " or " +
            std::string(jss::seed_hex));
        return { };
    }

    boost::optional<KeyType> keyType;
    boost::optional<Seed> seed;

    if (has_key_type)
    {
        if (! params[jss::key_type].isString())
        {
            error = RPC::expected_field_error (
                jss::key_type, "string");
            return { };
        }

        keyType = keyTypeFromString(params[jss::key_type].asString());

        if (!keyType)
        {
            error = RPC::invalid_field_error(jss::key_type);
            return { };
        }

        if (secretType == jss::secret.c_str())
        {
            error = RPC::make_param_error (
                "The secret field is not allowed if " +
                std::string(jss::key_type) + " is used.");
            return { };
        }
    }

    // ripple-lib encodes seed used to generate an Ed25519 wallet in a
    // non-standard way. While we never encode seeds that way, we try
    // to detect such keys to avoid user confusion.
    if (secretType != jss::seed_hex.c_str())
    {
        seed = RPC::parseRippleLibSeed(params[secretType]);

        if (seed)
        {
            // If the user passed in an Ed25519 seed but *explicitly*
            // requested another key type, return an error.
            if (keyType.value_or(KeyType::ed25519) != KeyType::ed25519)
            {
                error = RPC::make_error (rpcBAD_SEED,
                    "Specified seed is for an Ed25519 wallet.");
                return { };
            }

            keyType = KeyType::ed25519;
        }
    }

    if (!keyType)
        keyType = KeyType::secp256k1;

    if (!seed)
    {
        if (has_key_type)
            seed = getSeedFromRPC(params, error);
        else
        {
            if (!params[jss::secret].isString())
            {
                error = RPC::expected_field_error(jss::secret, "string");
                return {};
            }

            seed = parseGenericSeed(params[jss::secret].asString());
        }
    }

    if (!seed)
    {
        if (!contains_error (error))
        {
            error = RPC::make_error (rpcBAD_SEED,
                RPC::invalid_field_message (secretType));
        }

        return { };
    }

    if (keyType != KeyType::secp256k1 && keyType != KeyType::ed25519)
        LogicError ("keypairForSignature: invalid key type");

    return generateKeyPair (*keyType, *seed);
}

std::pair<RPC::Status, LedgerEntryType>
chooseLedgerEntryType(Json::Value const& params)
{
    std::pair<RPC::Status, LedgerEntryType> result{ RPC::Status::OK, ltINVALID };
    if (params.isMember(jss::type))
    {
        static
            std::array<std::pair<char const *, LedgerEntryType>, 13> const types
        { {
            { jss::account,         ltACCOUNT_ROOT },
            { jss::amendments,      ltAMENDMENTS },
            { jss::check,           ltCHECK },
            { jss::deposit_preauth, ltDEPOSIT_PREAUTH },
            { jss::directory,       ltDIR_NODE },
            { jss::escrow,          ltESCROW },
            { jss::fee,             ltFEE_SETTINGS },
            { jss::hashes,          ltLEDGER_HASHES },
            { jss::offer,           ltOFFER },
            { jss::payment_channel, ltPAYCHAN },
            { jss::signer_list,     ltSIGNER_LIST },
            { jss::state,           ltRIPPLE_STATE },
            { jss::ticket,          ltTICKET }
            } };

        auto const& p = params[jss::type];
        if (!p.isString())
        {
            result.first = RPC::Status{ rpcINVALID_PARAMS,
                "Invalid field 'type', not string." };
            assert(result.first.type() == RPC::Status::Type::error_code_i);
            return result;
        }

        auto const filter = p.asString();
        auto iter = std::find_if(types.begin(), types.end(),
            [&filter](decltype (types.front())& t)
        {
            return t.first == filter;
        });
        if (iter == types.end())
        {
            result.first = RPC::Status{ rpcINVALID_PARAMS,
                "Invalid field 'type'." };
            assert(result.first.type() == RPC::Status::Type::error_code_i);
            return result;
        }
        result.second = iter->second;
    }
    return result;
}

// Only populate the protobuf field if the field is present in obj
template <class FieldType>
void
populateIfPresent(
    STObject const& obj,
    FieldType const& field,
    std::function<void(STObject const&,FieldType const&)>const & populate)
{
    if (obj.isFieldPresent(field))
        populate(obj, field);
}

void
populateAccountSet(rpc::v1::AccountSet& proto, STObject const& obj)
{
    populateClearFlag(obj, proto);

    populateDomain(obj, proto);

    populateEmailHash(obj, proto);

    populateMessageKey(obj, proto);

    populateSetFlag(obj, proto);

    populateTransferRate(obj, proto);

    populateTickSize(obj, proto);
}

void
populateOfferCreate(rpc::v1::OfferCreate& proto, STObject const& obj)
{
    populateExpiration(obj, proto);

    populateOfferSequence(obj, proto);

    populateTakerGets(obj, proto);

    populateTakerPays(obj, proto);
}

void
populateOfferCancel(rpc::v1::OfferCancel& proto, STObject const& obj)
{
    populateOfferSequence(obj, proto);
}

void
populateAccountDelete(rpc::v1::AccountDelete& proto, STObject const& obj)
{
    populateDestination(obj, proto);
}

void
populateCheckCancel(rpc::v1::CheckCancel& proto, STObject const& obj)
{
    populateCheckID(obj, proto);
}

void
populateCheckCash(rpc::v1::CheckCash& proto, STObject const& obj)
{
    populateCheckID(obj, proto);

    populateAmount(obj, proto);

    populateDeliverMin(obj, proto);
}

void
populateCheckCreate(rpc::v1::CheckCreate& proto, STObject const& obj)
{
    populateDestination(obj, proto);

    populateSendMax(obj, proto);

    populateDestinationTag(obj, proto);

    populateExpiration(obj, proto);

    populateInvoiceID(obj, proto);
}

void
populateDepositPreauth(rpc::v1::DepositPreauth& proto, STObject const& obj)
{
    populateAuthorize(obj, proto);

    populateUnauthorize(obj, proto);
}

void
populateEscrowCancel(rpc::v1::EscrowCancel& proto, STObject const& obj)
{
    populateOwner(obj, proto);

    populateOfferSequence(obj, proto);
}

void
populateEscrowCreate(rpc::v1::EscrowCreate& proto, STObject const& obj)
{
    populateAmount(obj, proto);

    populateDestination(obj, proto);

    populateCancelAfter(obj, proto);

    populateFinishAfter(obj, proto);

    populateCondition(obj, proto);

    populateDestinationTag(obj, proto);
}

void
populateEscrowFinish(rpc::v1::EscrowFinish& proto, STObject const& obj)
{
    populateOwner(obj, proto);

    populateOfferSequence(obj, proto);

    populateCondition(obj, proto);

    populateFulfillment(obj, proto);
}

void
populatePaymentChannelClaim(
    rpc::v1::PaymentChannelClaim& proto,
    STObject const& obj)
{
    populateChannel(obj, proto);

    populateBalance(obj, proto);

    populateAmount(obj, proto);

    populateSignature(obj, proto);

    populatePublicKey(obj, proto);
}

void
populatePaymentChannelCreate(
    rpc::v1::PaymentChannelCreate& proto,
    STObject const& obj)
{

    populateAmount(obj, proto);

    populateDestination(obj, proto);

    populateSettleDelay(obj, proto);

    populatePublicKey(obj, proto);

    populateCancelAfter(obj, proto);

    populateDestinationTag(obj, proto);
}

void
populatePaymentChannelFund(
    rpc::v1::PaymentChannelFund& proto,
    STObject const& obj)
{

    populateChannel(obj, proto);

    populateAmount(obj, proto);

    populateExpiration(obj, proto);
}

void
populateSetRegularKey(rpc::v1::SetRegularKey& proto, STObject const& obj)
{
    populateRegularKey(obj, proto);
}

void
populateSignerListSet(rpc::v1::SignerListSet& proto, STObject const& obj)
{
    populateSignerQuorum(obj, proto);

    populateSignerEntries(obj, proto);
}

void
populateTrustSet(rpc::v1::TrustSet& proto, STObject const& obj)
{
    populateLimitAmount(obj, proto);

    populateQualityIn(obj, proto);

    populateQualityOut(obj, proto);
}

void
populatePayment(rpc::v1::Payment& proto, STObject const& obj)
{
    populateAmount(obj, proto);

    populateDestination(obj, proto);

    populateDestinationTag(obj, proto);

    populateInvoiceID(obj, proto);

    populateSendMax(obj, proto);

    populateDeliverMin(obj, proto);

    //TODO change this code to follow the new pattern, if possible
    if(obj.isFieldPresent(sfPaths))
    {
        // populate path data
        STPathSet const& pathset = obj.getFieldPathSet(sfPaths);
        for (auto it = pathset.begin(); it < pathset.end(); ++it)
        {
            STPath const& path = *it;

            rpc::v1::Path* protoPath = proto.add_paths();

            for (auto it2 = path.begin(); it2 != path.end(); ++it2)
            {
                rpc::v1::PathElement* protoElement = protoPath->add_elements();
                STPathElement const& elt = *it2;

                if (elt.isOffer())
                {
                    if (elt.hasCurrency())
                    {
                        Currency const& currency = elt.getCurrency();
                        protoElement->mutable_currency()->set_name(
                            to_string(currency));
                    }
                    if (elt.hasIssuer())
                    {
                        AccountID const& issuer = elt.getIssuerID();
                        protoElement->mutable_issuer()->set_address(
                            toBase58(issuer));
                    }
                }
                else if(elt.isAccount())
                {
                    AccountID const& pathAccount = elt.getAccountID();
                    protoElement->mutable_account()->set_address(
                        toBase58(pathAccount));
                }
            }
        }
    }
}

void
populateAccountRoot(rpc::v1::AccountRoot& proto, STObject const& obj)
{

    populateAccount(obj, proto);

    populateBalance(obj, proto);

    populateSequence(obj, proto);

    populateFlags(obj, proto);

    populateOwnerCount(obj, proto);

    populatePreviousTransactionID(obj, proto);

    populatePreviousTransactionLedgerSequence(obj, proto);

    populateAccountTransactionID(obj, proto);

    populateDomain(obj, proto);

    populateEmailHash(obj, proto);

    populateMessageKey(obj, proto);

    populateRegularKey(obj, proto);

    populateTickSize(obj, proto);

    populateTransferRate(obj, proto);
}

void
populateAmendments(rpc::v1::Amendments& proto, STObject const& obj)
{

    populateAmendments(obj, proto);

    populateMajorities(obj, proto);
}

void populateCheck(rpc::v1::Check& proto, STObject const& obj)
{
   populateAccount(obj, proto);

    populateDestination(obj, proto);

    populateFlags(obj, proto);

    populateOwnerNode(obj, proto);

    populatePreviousTransactionID(obj, proto);

    populatePreviousTransactionLedgerSequence(obj, proto);

    populateSendMax(obj, proto);

    populateSequence(obj, proto);

    populateDestinationNode(obj, proto);

    populateDestinationTag(obj, proto);

    populateExpiration(obj, proto);

    populateInvoiceID(obj, proto);

    populateSourceTag(obj, proto);
}

void populateDepositPreauth(rpc::v1::DepositPreauthObject& proto, STObject const& obj)
{
    populateAccount(obj, proto);

    populateAuthorize(obj, proto);

    populateFlags(obj, proto);

    populateOwnerNode(obj, proto);

    populatePreviousTransactionID(obj, proto);

    populatePreviousTransactionLedgerSequence(obj, proto);
}

void populateFeeSettings(rpc::v1::FeeSettings& proto, STObject const& obj)
{
   populateBaseFee(obj, proto);

   populateReferenceFeeUnits(obj, proto);

   populateReserveBase(obj, proto);

   populateReserveIncrement(obj, proto);

   populateFlags(obj, proto);
}


void populateEscrow(rpc::v1::Escrow& proto, STObject const& obj)
{
    populateAccount(obj, proto);

    populateDestination(obj, proto);

    populateAmount(obj, proto);

    populateCondition(obj, proto);

    populateCancelAfter(obj, proto);

    populateFinishAfter(obj, proto);

    populateFlags(obj, proto);

    populateSourceTag(obj, proto);

    populateDestinationTag(obj, proto);

    populateOwnerNode(obj, proto);

    populateDestinationNode(obj, proto);

    populatePreviousTransactionID(obj, proto);

    populatePreviousTransactionLedgerSequence(obj, proto);

}

void
populateLedgerHashes(rpc::v1::LedgerHashes& proto, STObject const& obj)
{
    populateLastLedgerSequence(obj, proto);

    populateHashes(obj, proto);

    populateFlags(obj, proto);
}

void
populatePayChannel(rpc::v1::PayChannel& proto, STObject const& obj)
{
    populateAccount(obj, proto);

    populateAmount(obj, proto);

    populateBalance(obj, proto);

    populatePublicKey(obj, proto);

    populateSettleDelay(obj, proto);

    populateOwnerNode(obj, proto);

    populatePreviousTransactionID(obj, proto);

    populatePreviousTransactionLedgerSequence(obj, proto);

    populateFlags(obj, proto);

    populateExpiration(obj, proto);

    populateCancelAfter(obj, proto);

    populateSourceTag(obj, proto);

    populateDestinationTag(obj, proto);
}

void
populateDirectoryNode(rpc::v1::DirectoryNode& proto, STObject const& obj)
{

    populateFlags(obj, proto);

    populateRootIndex(obj, proto);

    populateIndexes(obj, proto);

    populateIndexNext(obj, proto);

    populateIndexPrevious(obj, proto);

    populateTakerPaysIssuer(obj, proto);

    populateTakerPaysCurrency(obj, proto);

    populateTakerGetsCurrency(obj, proto);

    populateTakerGetsIssuer(obj, proto);
}

void
populateOffer(rpc::v1::Offer& proto, STObject const& obj)
{

    populateAccount(obj, proto);

    populateSequence(obj, proto);

    populateFlags(obj, proto);

    populateTakerPays(obj, proto);

    populateTakerGets(obj, proto);

    populateBookDirectory(obj, proto);

    populateBookNode(obj, proto);
}

void
populateRippleState(rpc::v1::RippleState& proto, STObject const& obj)
{

    populateBalance(obj, proto);

    populateFlags(obj, proto);

    populateLowNode(obj, proto);

    populateHighNode(obj, proto);

    populateLowQualityIn(obj, proto);

    populateLowQualityOut(obj, proto);

    populateHighQualityIn(obj, proto);

    populateHighQualityOut(obj, proto);

    populatePreviousTransactionID(obj, proto);

    populatePreviousTransactionLedgerSequence(obj, proto);
}

void
populateSignerList(rpc::v1::SignerList& proto, STObject const& obj)
{

    populateFlags(obj, proto);

    populatePreviousTransactionID(obj, proto);

    populatePreviousTransactionLedgerSequence(obj, proto);

    populateOwnerNode(obj, proto);

    populateSignerEntries(obj, proto);

    populateSignerQuorum(obj, proto);

    populateSignerListID(obj, proto);
}




void
populateLedgerEntryType(rpc::v1::AffectedNode& proto, std::uint16_t lgrType)
{
    switch (lgrType)
    {
        case ltACCOUNT_ROOT:
            proto.set_ledger_entry_type(
                rpc::v1::LEDGER_ENTRY_TYPE_ACCOUNT_ROOT);
            break;
        case ltDIR_NODE:
            proto.set_ledger_entry_type(
                rpc::v1::LEDGER_ENTRY_TYPE_DIRECTORY_NODE);
            break;
        case ltRIPPLE_STATE:
            proto.set_ledger_entry_type(
                rpc::v1::LEDGER_ENTRY_TYPE_RIPPLE_STATE);
            break;
        case ltSIGNER_LIST:
            proto.set_ledger_entry_type(rpc::v1::LEDGER_ENTRY_TYPE_SIGNER_LIST);
            break;
        case ltOFFER:
            proto.set_ledger_entry_type(rpc::v1::LEDGER_ENTRY_TYPE_OFFER);
            break;
        case ltLEDGER_HASHES:
            proto.set_ledger_entry_type(
                rpc::v1::LEDGER_ENTRY_TYPE_LEDGER_HASHES);
            break;
        case ltAMENDMENTS:
            proto.set_ledger_entry_type(rpc::v1::LEDGER_ENTRY_TYPE_AMENDMENTS);
            break;
        case ltFEE_SETTINGS:
            proto.set_ledger_entry_type(
                rpc::v1::LEDGER_ENTRY_TYPE_FEE_SETTINGS);
            break;
        case ltESCROW:
            proto.set_ledger_entry_type(rpc::v1::LEDGER_ENTRY_TYPE_ESCROW);
            break;
        case ltPAYCHAN:
            proto.set_ledger_entry_type(rpc::v1::LEDGER_ENTRY_TYPE_PAY_CHANNEL);
            break;
        case ltCHECK:
            proto.set_ledger_entry_type(rpc::v1::LEDGER_ENTRY_TYPE_CHECK);
            break;
        case ltDEPOSIT_PREAUTH:
            proto.set_ledger_entry_type(
                rpc::v1::LEDGER_ENTRY_TYPE_DEPOSIT_PREAUTH);
            break;
    }
}

template <class T>
void
populateFields(T& proto, STObject const& obj, std::uint16_t type)
{
    switch (type)
    {
        case ltACCOUNT_ROOT:
            RPC::populateAccountRoot(*proto.mutable_account_root(), obj);
            break;
        case ltAMENDMENTS:
            RPC::populateAmendments(*proto.mutable_amendments(), obj);
            break;
        case ltDIR_NODE:
            RPC::populateDirectoryNode(*proto.mutable_directory_node(), obj);
            break;
        case ltRIPPLE_STATE:
            RPC::populateRippleState(*proto.mutable_ripple_state(), obj);
            break;
        case ltSIGNER_LIST:
            RPC::populateSignerList(*proto.mutable_signer_list(), obj);
            break;
        case ltOFFER:
            RPC::populateOffer(*proto.mutable_offer(), obj);
            break;
        case ltLEDGER_HASHES:
            RPC::populateLedgerHashes(*proto.mutable_ledger_hashes(), obj);
            break;
        case ltFEE_SETTINGS:
            RPC::populateFeeSettings(*proto.mutable_fee_settings(), obj);
            break;
        case ltESCROW:
            RPC::populateEscrow(*proto.mutable_escrow(), obj);
            break;
        case ltPAYCHAN:
            RPC::populatePayChannel(*proto.mutable_pay_channel(), obj);
            break;
        case ltCHECK:
            RPC::populateCheck(*proto.mutable_check(), obj);
            break;
        case ltDEPOSIT_PREAUTH:
            RPC::populateDepositPreauth(*proto.mutable_deposit_preauth(), obj);
            break;
    }
}

void
populateMeta(rpc::v1::Meta& proto, std::shared_ptr<TxMeta> txMeta)
{

    std::cout << "populating meta" << std::endl;
    proto.set_transaction_index(txMeta->getIndex());

    populateTransactionResultType(
        *proto.mutable_transaction_result(), txMeta->getResultTER());
    proto.mutable_transaction_result()->set_result(
        transToken(txMeta->getResultTER()));

    STArray& nodes = txMeta->getNodes();
    for (auto it = nodes.begin(); it != nodes.end(); ++it)
    {
        STObject& obj = *it;
        rpc::v1::AffectedNode* node = proto.add_affected_nodes();

        // ledger index
        uint256 ledgerIndex = obj.getFieldH256(sfLedgerIndex);
        node->set_ledger_index(ledgerIndex.data(), ledgerIndex.size());

        // ledger entry type
        std::uint16_t lgrType = obj.getFieldU16(sfLedgerEntryType);
        populateLedgerEntryType(*node, lgrType);

        // modified node
        if (obj.getFName() == sfModifiedNode)
        {
            // final fields
            if (obj.isFieldPresent(sfFinalFields))
            {
                STObject& finalFields =
                    obj.getField(sfFinalFields).downcast<STObject>();

                rpc::v1::LedgerObject* finalFieldsProto =
                    node->mutable_modified_node()->mutable_final_fields();

                populateFields(*finalFieldsProto, finalFields, lgrType);
            }
            // previous fields
            if (obj.isFieldPresent(sfPreviousFields))
            {
                STObject& prevFields =
                    obj.getField(sfPreviousFields).downcast<STObject>();

                rpc::v1::LedgerObject* prevFieldsProto =
                    node->mutable_modified_node()->mutable_previous_fields();

                populateFields(*prevFieldsProto, prevFields, lgrType);
            }

            // prev txn id and prev txn ledger seq
            if(obj.isFieldPresent(sfPreviousTxnID))
            {
                uint256 prevTxnId = obj.getFieldH256(sfPreviousTxnID);
                node->mutable_modified_node()->set_previous_transaction_id(
                        prevTxnId.data(), prevTxnId.size());
            }
            if(obj.isFieldPresent(sfPreviousTxnLgrSeq))
            {
                node->mutable_modified_node()
                    ->set_previous_transaction_ledger_sequence(
                            obj.getFieldU32(sfPreviousTxnLgrSeq));
            }
        }
        // created node
        else if (obj.getFName() == sfCreatedNode)
        {
            // new fields
            if (obj.isFieldPresent(sfNewFields))
            {
                STObject& newFields =
                    obj.getField(sfNewFields).downcast<STObject>();

                rpc::v1::LedgerObject* newFieldsProto =
                    node->mutable_created_node()->mutable_new_fields();

                populateFields(*newFieldsProto, newFields, lgrType);
            }
        }
        // deleted node
        else if (obj.getFName() == sfDeletedNode)
        {
            // final fields
            if (obj.isFieldPresent(sfFinalFields))
            {
                STObject& finalFields =
                    obj.getField(sfFinalFields).downcast<STObject>();

                rpc::v1::LedgerObject* finalFieldsProto =
                    node->mutable_deleted_node()->mutable_final_fields();

                populateFields(*finalFieldsProto, finalFields, lgrType);
            }
        }
    }
}


void
populateQueueData(
    rpc::v1::QueueData& proto,
    std::map<TxSeq, TxQ::AccountTxDetails const> const& txs)
{
    if (!txs.empty())
    {
        proto.set_txn_count(txs.size());
        proto.set_lowest_sequence(txs.begin()->first);
        proto.set_highest_sequence(txs.rbegin()->first);

        boost::optional<bool> anyAuthChanged(false);
        boost::optional<XRPAmount> totalSpend(0);

        for (auto const& [txSeq, txDetails] : txs)
        {
            rpc::v1::QueuedTransaction& qt = *proto.add_transactions();

            qt.set_sequence(txSeq);
            qt.set_fee_level(txDetails.feeLevel.fee());
            if (txDetails.lastValid)
                qt.set_last_ledger_sequence(*txDetails.lastValid);

            if (txDetails.consequences)
            {
                qt.mutable_fee()->set_drops(
                    txDetails.consequences->fee.drops());
                auto spend = txDetails.consequences->potentialSpend +
                    txDetails.consequences->fee;
                qt.mutable_max_spend_drops()->set_drops(spend.drops());
                if (totalSpend)
                    *totalSpend += spend;
                auto authChanged =
                    txDetails.consequences->category == TxConsequences::blocker;
                if (authChanged)
                    anyAuthChanged.emplace(authChanged);
                qt.set_auth_change(authChanged);
            }
            else
            {
                if (anyAuthChanged && !*anyAuthChanged)
                    anyAuthChanged.reset();
                totalSpend.reset();
            }
        }

        if (anyAuthChanged)
            proto.set_auth_change_queued(*anyAuthChanged);
        if (totalSpend)
            proto.mutable_max_spend_drops_total()->set_drops(
                (*totalSpend).drops());
    }
}

void
populateAmount(rpc::v1::CurrencyAmount& proto, STAmount const& amount)
{
    if (amount.native())
    {
        proto.mutable_xrp_amount()->set_drops(amount.xrp().drops());
    }
    else
    {
        rpc::v1::IssuedCurrencyAmount* issued =
            proto.mutable_issued_currency_amount();
        Issue const& issue = amount.issue();
        Currency currency = issue.currency;
        issued->mutable_currency()->set_name(to_string(issue.currency));
        issued->mutable_currency()->set_code(currency.data(), currency.size());
        issued->set_value(to_string(amount.iou()));
        issued->mutable_issuer()->set_address(toBase58(issue.account));
    }
}

void
populateTransaction(
    rpc::v1::Transaction& proto,
    std::shared_ptr<STTx const> txnSt)
{
    STObject const& obj = *txnSt;

    populateAccount(obj, proto);

    populateFee(obj, proto);

    populateSequence(obj, proto);

    populateSigningPublicKey(obj, proto);

    populateTransactionSignature(obj, proto);

    populateFlags(obj, proto);

    populateLastLedgerSequence(obj, proto);

    populateSourceTag(obj, proto);

    populateAccountTransactionID(obj, proto);

    populateMemos(obj, proto);

    populateSigners(obj, proto);

    auto type = safe_cast<TxType>(txnSt->getFieldU16(sfTransactionType));

    switch (type)
    {
        case TxType::ttPAYMENT:
            populatePayment(*proto.mutable_payment(), *txnSt);
            break;
        case TxType::ttESCROW_CREATE:
            populateEscrowCreate(*proto.mutable_escrow_create(), *txnSt);
            break;
        case TxType::ttESCROW_FINISH:
            populateEscrowFinish(*proto.mutable_escrow_finish(), *txnSt);
            break;
        case TxType::ttACCOUNT_SET:
            populateAccountSet(*proto.mutable_account_set(), *txnSt);
            break;
        case TxType::ttESCROW_CANCEL:
            populateEscrowCancel(*proto.mutable_escrow_cancel(), *txnSt);
            break;
        case TxType::ttREGULAR_KEY_SET:
            populateSetRegularKey(*proto.mutable_set_regular_key(), *txnSt);
            break;
        case TxType::ttOFFER_CREATE:
            populateOfferCreate(*proto.mutable_offer_create(), *txnSt);
            break;
        case TxType::ttOFFER_CANCEL:
            populateOfferCancel(*proto.mutable_offer_cancel(), *txnSt);
            break;
        case TxType::ttSIGNER_LIST_SET:
            populateSignerListSet(*proto.mutable_signer_list_set(), *txnSt);
            break;
        case TxType::ttPAYCHAN_CREATE:
            populatePaymentChannelCreate(
                *proto.mutable_payment_channel_create(), *txnSt);
            break;
        case TxType::ttPAYCHAN_FUND:
            populatePaymentChannelFund(
                *proto.mutable_payment_channel_fund(), *txnSt);
            break;
        case TxType::ttPAYCHAN_CLAIM:
            populatePaymentChannelClaim(
                *proto.mutable_payment_channel_claim(), *txnSt);
            break;
        case TxType::ttCHECK_CREATE:
            populateCheckCreate(*proto.mutable_check_create(), *txnSt);
            break;
        case TxType::ttCHECK_CASH:
            populateCheckCash(*proto.mutable_check_cash(), *txnSt);
            break;
        case TxType::ttCHECK_CANCEL:
            populateCheckCancel(*proto.mutable_check_cancel(), *txnSt);
            break;
        case TxType::ttDEPOSIT_PREAUTH:
            populateDepositPreauth(*proto.mutable_deposit_preauth(), *txnSt);
            break;
        case TxType::ttTRUST_SET:
            populateTrustSet(*proto.mutable_trust_set(), *txnSt);
            break;
        case TxType::ttACCOUNT_DELETE:
            populateAccountDelete(*proto.mutable_account_delete(), *txnSt);
            break;
        default:
            break;
    }
}

void
populateTransactionResultType(rpc::v1::TransactionResult& proto, TER result)
{
    if (isTecClaim(result))
    {
        proto.set_result_type(rpc::v1::TransactionResult::RESULT_TYPE_TEC);
    }
    if (isTefFailure(result))
    {
        proto.set_result_type(rpc::v1::TransactionResult::RESULT_TYPE_TEF);
    }
    if (isTelLocal(result))
    {
        proto.set_result_type(rpc::v1::TransactionResult::RESULT_TYPE_TEL);
    }
    if (isTemMalformed(result))
    {
        proto.set_result_type(rpc::v1::TransactionResult::RESULT_TYPE_TEM);
    }
    if (isTerRetry(result))
    {
        proto.set_result_type(rpc::v1::TransactionResult::RESULT_TYPE_TER);
    }
    if (isTesSuccess(result))
    {
        proto.set_result_type(rpc::v1::TransactionResult::RESULT_TYPE_TES);
    }
}

beast::SemanticVersion const firstVersion("1.0.0");
beast::SemanticVersion const goodVersion("1.0.0");
beast::SemanticVersion const lastVersion("1.0.0");

unsigned int getAPIVersionNumber(Json::Value const& jv)
{
    static Json::Value const minVersion (RPC::ApiMinimumSupportedVersion);
    static Json::Value const maxVersion (RPC::ApiMaximumSupportedVersion);
    static Json::Value const invalidVersion (RPC::APIInvalidVersion);

    Json::Value requestedVersion(RPC::APIVersionIfUnspecified);
    if(jv.isObject())
    {
        requestedVersion = jv.get (jss::api_version, requestedVersion);
    }
    if( !(requestedVersion.isInt() || requestedVersion.isUInt()) ||
        requestedVersion < minVersion || requestedVersion > maxVersion)
    {
        requestedVersion = invalidVersion;
    }
    return requestedVersion.asUInt();
}

} // RPC
} // ripple
