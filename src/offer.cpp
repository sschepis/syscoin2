#include "offer.h"
#include "alias.h"
#include "escrow.h"
#include "cert.h"
#include "message.h"
#include "init.h"
#include "main.h"
#include "util.h"
#include "random.h"
#include "base58.h"
#include "rpcserver.h"
#include "wallet/wallet.h"
#include "consensus/validation.h"
#include "chainparams.h"
#include <boost/algorithm/string/case_conv.hpp> // for to_lower()
#include <boost/xpressive/xpressive_dynamic.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/foreach.hpp>
#include <boost/thread.hpp>
#include <boost/algorithm/string/predicate.hpp>
using namespace std;
extern void SendMoney(const CTxDestination &address, CAmount nValue, bool fSubtractFeeFromAmount, CWalletTx& wtxNew);
extern void SendMoneySyscoin(const vector<CRecipient> &vecSend, CAmount nValue, bool fSubtractFeeFromAmount, CWalletTx& wtxNew, const CWalletTx* wtxInOffer=NULL, const CWalletTx* wtxInCert=NULL, const CWalletTx* wtxInAlias=NULL, const CWalletTx* wtxInEscrow=NULL, bool syscoinTx=true, string justcheck="0");
bool DisconnectAlias(const CBlockIndex *pindex, const CTransaction &tx, int op, vector<vector<unsigned char> > &vvchArgs );
bool DisconnectOffer(const CBlockIndex *pindex, const CTransaction &tx, int op, vector<vector<unsigned char> > &vvchArgs );
bool DisconnectCertificate(const CBlockIndex *pindex, const CTransaction &tx, int op, vector<vector<unsigned char> > &vvchArgs );
bool DisconnectMessage(const CBlockIndex *pindex, const CTransaction &tx, int op, vector<vector<unsigned char> > &vvchArgs );
bool DisconnectEscrow(const CBlockIndex *pindex, const CTransaction &tx, int op, vector<vector<unsigned char> > &vvchArgs );
static const CBlock *linkedAcceptBlock = NULL;
bool foundOfferLinkInWallet(const vector<unsigned char> &vchOffer, const vector<unsigned char> &vchAcceptRandLink)
{
    TRY_LOCK(pwalletMain->cs_wallet, cs_trylock);
    BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, pwalletMain->mapWallet)
    {
		vector<vector<unsigned char> > vvchArgs;
		int op, nOut;
        const CWalletTx& wtx = item.second;
        if (wtx.IsCoinBase() || !CheckFinalTx(wtx))
            continue;
		if(wtx.nVersion != GetSyscoinTxVersion())
			continue;
		if (DecodeOfferTx(wtx, op, nOut, vvchArgs))
		{
			if(op == OP_OFFER_ACCEPT)
			{
				if(vvchArgs[0] == vchOffer)
				{
					vector<unsigned char> vchOfferAcceptLink;
					bool foundOffer = false;
					for (unsigned int i = 0; i < wtx.vin.size(); i++) {
						vector<vector<unsigned char> > vvchIn;
						int opIn;
						const COutPoint *prevOutput = &wtx.vin[i].prevout;
						if(!GetPreviousInput(prevOutput, opIn, vvchIn))
							continue;
						if(foundOffer)
							break;

						if (!foundOffer && opIn == OP_OFFER_ACCEPT && vvchIn[2] != vchFromString("1")) {
							foundOffer = true; 
							vchOfferAcceptLink = vvchIn[1];
						}
					}
					if(vchOfferAcceptLink == vchAcceptRandLink)
						return true;				
				}
			}
		}
	}
	return false;
}
// transfer cert if its linked to offer
string makeTransferCertTX(const COffer& theOffer, const COfferAccept& theOfferAccept)
{

	string strError;
	string strMethod = string("certtransfer");
	UniValue params(UniValue::VARR);
	params.push_back(stringFromVch(theOffer.vchCert));
	params.push_back(stringFromVch(theOfferAccept.vchBuyerAlias));
    try {
        tableRPC.execute(strMethod, params);
	}
	catch (UniValue& objError)
	{
		return find_value(objError, "message").get_str().c_str();
	}
	catch(std::exception& e)
	{
		return string(e.what()).c_str();
	}
	return "";

}
// refund an offer accept by creating a transaction to send coins to offer accepter, and an offer accept back to the offer owner. 2 Step process in order to use the coins that were sent during initial accept.
string makeOfferLinkAcceptTX(const COfferAccept& theOfferAccept, const vector<unsigned char> &vchOffer, const vector<unsigned char> &vchMessage, const vector<unsigned char> &vchLinkOffer, const string &offerAcceptLinkTxHash, const COffer& theOffer, const CAliasIndex& theAlias, const CBlock* block)
{
	if(!block)
	{
		return "cannot accept linked offer with no linkedAcceptBlock defined";
	}
	string strError;
	string strMethod = string("offeraccept");
	UniValue params(UniValue::VARR);

	CPubKey newDefaultKey;
	linkedAcceptBlock = block;
	vector<vector<unsigned char> > vvchArgs;
	bool foundOfferTx = false;
	for (unsigned int i = 0; i < linkedAcceptBlock->vtx.size(); i++)
    {
		if(foundOfferTx)
			break;
        const CTransaction &tx = linkedAcceptBlock->vtx[i];
		if(tx.nVersion == GetSyscoinTxVersion())
		{
			
			int op, nOut;			
			bool good = true;
			// find first offer accept in this block and make sure it is for the linked offer we are checking
			// the first one is the one that is used to do the offer accept tx, so any subsequent accept tx for the same offer will also check this tx and find that
			// the linked accept tx was already done (grouped all accept's together in this block)
			if(DecodeOfferTx(tx, op, nOut, vvchArgs) && op == OP_OFFER_ACCEPT && vvchArgs[0] == vchOffer)
			{	
				foundOfferTx = true;
				if(foundOfferLinkInWallet(vchLinkOffer, vvchArgs[1]))
				{
					return "offer linked transaction already exists";
				}
				break;
			}
		}
	}
	if(!foundOfferTx)
		return "cannot accept a linked offer accept in linkedAcceptBlock";

	CPubKey PubKey(theAlias.vchPubKey);
	params.push_back(stringFromVch(theOffer.vchAlias));
	params.push_back(stringFromVch(vchLinkOffer));
	params.push_back("99");
	params.push_back(stringFromVch(vchMessage));
	params.push_back("");
	params.push_back(offerAcceptLinkTxHash);
	
    try {
        tableRPC.execute(strMethod, params);
	}
	catch (UniValue& objError)
	{
		linkedAcceptBlock = NULL;
		return find_value(objError, "message").get_str().c_str();
	}
	catch(std::exception& e)
	{
		linkedAcceptBlock = NULL;
		return string(e.what()).c_str();
	}
	linkedAcceptBlock = NULL;
	return "";

}

bool IsOfferOp(int op) {
	return op == OP_OFFER_ACTIVATE
        || op == OP_OFFER_UPDATE
        || op == OP_OFFER_ACCEPT;
}


int GetOfferExpirationDepth() {
	#ifdef ENABLE_DEBUGRPC
    return 1440;
  #else
    return 525600;
  #endif
}

string offerFromOp(int op) {
	switch (op) {
	case OP_OFFER_ACTIVATE:
		return "offeractivate";
	case OP_OFFER_UPDATE:
		return "offerupdate";
	case OP_OFFER_ACCEPT:
		return "offeraccept";
	default:
		return "<unknown offer op>";
	}
}
bool COffer::UnserializeFromData(const vector<unsigned char> &vchData) {
    try {
        CDataStream dsOffer(vchData, SER_NETWORK, PROTOCOL_VERSION);
        dsOffer >> *this;
    } catch (std::exception &e) {
		SetNull();
        return false;
    }
	// extra check to ensure data was parsed correctly
	if(!IsValidAliasName(vchAlias))
	{
		SetNull();
		return false;
	}
	return true;
}
bool COffer::UnserializeFromTx(const CTransaction &tx) {
	vector<unsigned char> vchData;
	if(!GetSyscoinData(tx, vchData))
	{
		SetNull();
		return false;
	}
	if(!UnserializeFromData(vchData))
	{
		return false;
	}
    return true;
}
const vector<unsigned char> COffer::Serialize() {
    CDataStream dsOffer(SER_NETWORK, PROTOCOL_VERSION);
    dsOffer << *this;
    const vector<unsigned char> vchData(dsOffer.begin(), dsOffer.end());
    return vchData;

}
bool COfferDB::ScanOffers(const std::vector<unsigned char>& vchOffer, const string& strRegexp, bool safeSearch,const string& strCategory, unsigned int nMax,
		std::vector<std::pair<std::vector<unsigned char>, COffer> >& offerScan) {
   // regexp
    using namespace boost::xpressive;
    smatch offerparts;
	smatch nameparts;
	string strRegexpLower = strRegexp;
	boost::algorithm::to_lower(strRegexpLower);
	sregex cregex = sregex::compile(strRegexpLower);
	int nMaxAge  = GetOfferExpirationDepth();
	boost::scoped_ptr<CDBIterator> pcursor(NewIterator());
	pcursor->Seek(make_pair(string("offeri"), vchOffer));
    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
		pair<string, vector<unsigned char> > key;
        try {
			if (pcursor->GetKey(key) && key.first == "offeri") {
            	vector<unsigned char> vchOffer = key.second;
                vector<COffer> vtxPos;
				pcursor->GetValue(vtxPos);
				
				if (vtxPos.empty()){
					pcursor->Next();
					continue;
				}
				const COffer &txPos = vtxPos.back();
  				if (chainActive.Tip()->nHeight - txPos.nHeight >= nMaxAge)
				{
					pcursor->Next();
					continue;
				}     
				// dont return sold out offers
				if(txPos.nQty <= 0 && txPos.nQty != -1)
				{
					pcursor->Next();
					continue;
				}
				if(txPos.safetyLevel >= SAFETY_LEVEL1)
				{
					if(safeSearch)
					{
						pcursor->Next();
						continue;
					}
					if(txPos.safetyLevel > SAFETY_LEVEL1)
					{
						pcursor->Next();
						continue;
					}
				}
				if(!txPos.safeSearch && safeSearch)
				{
					pcursor->Next();
					continue;
				}
				if(strCategory.size() > 0 && !boost::algorithm::starts_with(stringFromVch(txPos.sCategory), strCategory))
				{
					pcursor->Next();
					continue;
				}

				string title = stringFromVch(txPos.sTitle);
				string offer = stringFromVch(vchOffer);
				boost::algorithm::to_lower(title);
				string description = stringFromVch(txPos.sDescription);
				boost::algorithm::to_lower(description);
				CAliasIndex theAlias;
				CTransaction aliastx;
				if(!GetTxOfAlias(txPos.vchAlias, theAlias, aliastx))
				{
					pcursor->Next();
					continue;
				}
				if(!theAlias.safeSearch && safeSearch)
				{
					pcursor->Next();
					continue;
				}
				if((safeSearch && theAlias.safetyLevel > txPos.safetyLevel) || (!safeSearch && theAlias.safetyLevel > SAFETY_LEVEL1))
				{
					pcursor->Next();
					continue;
				}
				string alias = stringFromVch(txPos.vchAlias);
				if (strRegexp != "" && !regex_search(title, offerparts, cregex) && !regex_search(description, offerparts, cregex) && strRegexp != offer && strRegexpLower != alias)
				{
					pcursor->Next();
					continue;
				}
				if(txPos.bPrivate)
				{
					if(strRegexp == "")
					{
						pcursor->Next();
						continue;
					}
					else if(strRegexp != offer)
					{
						pcursor->Next();
						continue;
					}
				}
                offerScan.push_back(make_pair(vchOffer, txPos));
            }
            if (offerScan.size() >= nMax)
                break;

            pcursor->Next();
        } catch (std::exception &e) {
            return error("%s() : deserialize error", __PRETTY_FUNCTION__);
        }
    }
    return true;
}

int IndexOfOfferOutput(const CTransaction& tx, bool skipAcceptBuyerSpecialOutput) {
	if (tx.nVersion != SYSCOIN_TX_VERSION)
		return -1;
	vector<vector<unsigned char> > vvch;
	int op;
	for (unsigned int i = 0; i < tx.vout.size(); i++) {
		const CTxOut& out = tx.vout[i];
		// find an output you own
		if (pwalletMain->IsMine(out) && DecodeOfferScript(out.scriptPubKey, op, vvch)) {
			if(skipAcceptBuyerSpecialOutput && op == OP_OFFER_ACCEPT && vvch[2] == vchFromString("1"))
				continue;
			return i;
		}
	}
	return -1;
}

bool GetTxOfOffer(const vector<unsigned char> &vchOffer, 
				  COffer& txPos, CTransaction& tx, bool skipExpiresCheck) {
	vector<COffer> vtxPos;
	if (!pofferdb->ReadOffer(vchOffer, vtxPos) || vtxPos.empty())
		return false;
	int nHeight = vtxPos.back().nHeight;
	txPos.nHeight = nHeight;
	if(!txPos.GetOfferFromList(vtxPos))
	{
		if(fDebug)
			LogPrintf("GetTxOfOffer() : cannot find offer from this offer position");
		return false;
	}
	if (!skipExpiresCheck && (nHeight + GetOfferExpirationDepth()
			< chainActive.Tip()->nHeight)) {
		string offer = stringFromVch(vchOffer);
		if(fDebug)
			LogPrintf("GetTxOfOffer(%s) : expired", offer.c_str());
		return false;
	}

	if (!GetSyscoinTransaction(txPos.nHeight, txPos.txHash, tx, Params().GetConsensus()))
		return false;

	return true;
}
bool GetTxAndVtxOfOffer(const vector<unsigned char> &vchOffer, 
				  COffer& txPos, CTransaction& tx, vector<COffer> &vtxPos, bool skipExpiresCheck) {
	if (!pofferdb->ReadOffer(vchOffer, vtxPos) || vtxPos.empty())
		return false;
	int nHeight = vtxPos.back().nHeight;
	txPos = vtxPos.back();
	
	if (!skipExpiresCheck && (nHeight + GetOfferExpirationDepth()
			< chainActive.Tip()->nHeight)) {
		string offer = stringFromVch(vchOffer);
		if(fDebug)
			LogPrintf("GetTxOfOffer(%s) : expired", offer.c_str());
		return false;
	}

	if (!GetSyscoinTransaction(txPos.nHeight, txPos.txHash, tx, Params().GetConsensus()))
		return false;

	return true;
}
bool GetTxOfOfferAccept(const vector<unsigned char> &vchOffer, const vector<unsigned char> &vchOfferAccept,
		COffer &theOffer, COfferAccept &theOfferAccept, CTransaction& tx, bool skipFeedback) {
	vector<COffer> vtxPos;
	if (!pofferdb->ReadOffer(vchOffer, vtxPos) || vtxPos.empty()) return false;
	theOfferAccept.SetNull();
	theOfferAccept.vchAcceptRand = vchOfferAccept;
	GetAcceptByHash(vtxPos, theOfferAccept, skipFeedback);
	if(theOfferAccept.IsNull())
		return false;
	int nHeight = theOfferAccept.nHeight;
	theOffer.nHeight = theOfferAccept.nAcceptHeight;
	if(!theOffer.GetOfferFromList(vtxPos))
	{
		if(fDebug)
			LogPrintf("GetTxOfOfferAccept() : cannot find offer from this offer accept position");
		return false;
	}
	if ((nHeight + GetOfferExpirationDepth())
			< chainActive.Tip()->nHeight) {
		string offer = stringFromVch(vchOfferAccept);
		if(fDebug)
			LogPrintf("GetTxOfOfferAccept(%s) : expired", offer.c_str());
		return false;
	}

	if (!GetSyscoinTransaction(nHeight, theOfferAccept.txHash, tx, Params().GetConsensus()))
		return false;

	return true;
}
bool DecodeAndParseOfferTx(const CTransaction& tx, int& op, int& nOut,
		vector<vector<unsigned char> >& vvch)
{
	COffer offer;
	bool decode = DecodeOfferTx(tx, op, nOut, vvch);
	bool parse = offer.UnserializeFromTx(tx);
	return decode && parse;
}
bool DecodeOfferTx(const CTransaction& tx, int& op, int& nOut,
		vector<vector<unsigned char> >& vvch) {
	bool found = false;

	// Strict check - bug disallowed
	for (unsigned int i = 0; i < tx.vout.size(); i++) {
		const CTxOut& out = tx.vout[i];
		// skip the special buyer feedback output (we should have the normal offer accept output also)
		if (DecodeOfferScript(out.scriptPubKey, op, vvch)) {
			if(op == OP_OFFER_ACCEPT && vvch[2] == vchFromString("1"))
				continue;
			nOut = i; found = true;
			break;
		}
	}
	if (!found) vvch.clear();
	return found && IsOfferOp(op);
}
bool FindOfferAcceptPayment(const CTransaction& tx, const CAmount &nPrice) {
	for (unsigned int i = 0; i < tx.vout.size(); i++) {
		if((tx.vout[i].nValue - nPrice) <= COIN)
			return true;
	}
	return false;
}

bool DecodeOfferScript(const CScript& script, int& op,
		vector<vector<unsigned char> > &vvch, CScript::const_iterator& pc) {
	opcodetype opcode;
	vvch.clear();
	if (!script.GetOp(pc, opcode)) return false;
	if (opcode < OP_1 || opcode > OP_16) return false;
	op = CScript::DecodeOP_N(opcode);

	for (;;) {
		vector<unsigned char> vch;
		if (!script.GetOp(pc, opcode, vch))
			return false;
		if (opcode == OP_DROP || opcode == OP_2DROP || opcode == OP_NOP)
			break;
		if (!(opcode >= 0 && opcode <= OP_PUSHDATA4))
			return false;
		vvch.push_back(vch);
	}

	// move the pc to after any DROP or NOP
	while (opcode == OP_DROP || opcode == OP_2DROP || opcode == OP_NOP) {
		if (!script.GetOp(pc, opcode))
			break;
	}

	pc--;
	return IsOfferOp(op);
}
bool DecodeOfferScript(const CScript& script, int& op,
		vector<vector<unsigned char> > &vvch) {
	CScript::const_iterator pc = script.begin();
	return DecodeOfferScript(script, op, vvch, pc);
}
CScript RemoveOfferScriptPrefix(const CScript& scriptIn) {
	int op;
	vector<vector<unsigned char> > vvch;
	CScript::const_iterator pc = scriptIn.begin();
	
	if (!DecodeOfferScript(scriptIn, op, vvch, pc))
	{
		throw runtime_error(
			"RemoveOfferScriptPrefix() : could not decode offer script");
	}

	return CScript(pc, scriptIn.end());
}

bool CheckOfferInputs(const CTransaction &tx, int op, int nOut, const vector<vector<unsigned char> > &vvchArgs, const CCoinsViewCache &inputs, bool fJustCheck, int nHeight, string &errorMessage, const CBlock* block, bool dontaddtodb) {
		
	if(!IsSys21Fork(nHeight))
		return true;
	if (tx.IsCoinBase())
		return true;
	if (fDebug)
		LogPrintf("*** OFFER %d %d %s %s %s %s %s %d\n", nHeight,
			chainActive.Tip()->nHeight, tx.GetHash().ToString().c_str(),
			op==OP_OFFER_ACCEPT ? "OFFERACCEPT: ": "", 
			op==OP_OFFER_ACCEPT ? stringFromVch(vvchArgs[1]).c_str(): "", 
			fJustCheck ? "JUSTCHECK" : "BLOCK", " VVCH SIZE: ", vvchArgs.size());
	bool foundOffer = false;
	bool foundCert = false;
	bool foundEscrow = false;
	bool foundAlias = false;
	const COutPoint *prevOutput = NULL;
	CCoins prevCoins;
	uint256 prevOfferHash;
	int prevOp, prevCertOp, prevEscrowOp, prevAliasOp;
	prevOp = prevCertOp = prevEscrowOp = prevAliasOp = 0;
	vector<vector<unsigned char> > vvchPrevArgs, vvchPrevCertArgs, vvchPrevEscrowArgs, vvchPrevAliasArgs;
	// unserialize msg from txn, check for valid
	COffer theOffer;
	vector<unsigned char> vchData;
	if(!GetSyscoinData(tx, vchData))
	{
		errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR ERRCODE: 1 - Cannot find data inside of this transaction relating to an offer";
		return true;
	}
	else if(!theOffer.UnserializeFromData(vchData))
	{
		errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR ERRCODE: 2 - Cannot unserialize data inside of this transaction relating to an offer";
		return true;
	}
	// Make sure offer outputs are not spent by a regular transaction, or the offer would be lost
	if (tx.nVersion != SYSCOIN_TX_VERSION) 
	{
		errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 3 - Non-Syscoin transaction found";
		return true;
	}
	if(fJustCheck)
	{
		
		if(op != OP_OFFER_ACCEPT && vvchArgs.size() != 2)
		{
			errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 4 - Offer arguments incorrect size";
			return error(errorMessage.c_str());
		}
		else if(op == OP_OFFER_ACCEPT && vvchArgs.size() != 4)
		{
			errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 5 - OfferAccept arguments incorrect size";
			return error(errorMessage.c_str());
		}
		if(!theOffer.IsNull())
		{
			uint256 calculatedHash = Hash(vchData.begin(), vchData.end());
			vector<unsigned char> vchRand = CScriptNum(calculatedHash.GetCheapHash()).getvch();
			vector<unsigned char> vchRandOffer = vchFromValue(HexStr(vchRand));
			if(op == OP_OFFER_ACCEPT)
			{
				if(vchRandOffer != vvchArgs[3])
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 6 - Hash provided doesn't match the calculated hash the data";
					return error(errorMessage.c_str());
				}
			}
			else
			{
				if(vchRandOffer != vvchArgs[1])
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 7 - Hash provided doesn't match the calculated hash the data";
					return error(errorMessage.c_str());
				}
			}
		}
			

		// Strict check - bug disallowed
		for (unsigned int i = 0; i < tx.vin.size(); i++) {
			vector<vector<unsigned char> > vvch;
			int pop;
			prevOutput = &tx.vin[i].prevout;
			if(!prevOutput)
				continue;
			// ensure inputs are unspent when doing consensus check to add to block
			if(!inputs.GetCoins(prevOutput->hash, prevCoins))
				continue;
			if(prevCoins.vout.size() <= prevOutput->n || !IsSyscoinScript(prevCoins.vout[prevOutput->n].scriptPubKey, pop, vvch))
				continue;


			if(foundEscrow && foundOffer && foundCert && foundAlias)
				break;

			if (!foundOffer && IsOfferOp(pop)) {
				foundOffer = true; 
				prevOp = pop;
				vvchPrevArgs = vvch;
				prevOfferHash = prevOutput->hash;
			}
			else if (!foundCert && IsCertOp(pop))
			{
				foundCert = true; 
				prevCertOp = pop;
				vvchPrevCertArgs = vvch;
			}
			else if (!foundEscrow && IsEscrowOp(pop))
			{
				foundEscrow = true; 
				prevEscrowOp = pop;
				vvchPrevEscrowArgs = vvch;
			}
			else if (!foundAlias && IsAliasOp(pop))
			{
				foundAlias = true; 
				prevAliasOp = pop;
				vvchPrevAliasArgs = vvch;
			}
		}
	
	}


	// unserialize offer from txn, check for valid
	COfferAccept theOfferAccept;
	vector<CAliasIndex> vtxAliasPos;
	COffer linkOffer;
	COffer myPriceOffer;
	COffer myOffer;
	CTransaction linkedTx;
	uint64_t heightToCheckAgainst;
	COfferLinkWhitelistEntry entry;
	CCert theCert;
	CAliasIndex theAlias, alias;
	CTransaction aliasTx;
	vector<COffer> vtxPos;
	bool linkAccept = false;
	bool escrowAccept = false;
	vector<string> rateList;
	vector<string> categories;
	string category;
	int precision = 2;
	CAmount nRate;
	string retError = "";
	// just check is for the memory pool inclusion, here we can stop bad transactions from entering before we get to include them in a block	
	if(fJustCheck)
	{
		if(theOffer.sDescription.size() > MAX_VALUE_LENGTH)
		{
			errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 8 - Offer description too long";
			return error(errorMessage.c_str());
		}
		if(theOffer.sTitle.size() > MAX_NAME_LENGTH)
		{
			errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 9 - Offer title too long";
			return error(errorMessage.c_str());
		}
		if(theOffer.sCategory.size() > MAX_NAME_LENGTH)
		{
			errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 10 - Offer category too long";
			return error(errorMessage.c_str());
		}
		if(theOffer.vchLinkOffer.size() > MAX_GUID_LENGTH)
		{
			errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 11 - Offer link guid hash too long";
			return error(errorMessage.c_str());
		}
		if(theOffer.sCurrencyCode.size() > MAX_GUID_LENGTH)
		{
			errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 13 - Offer curreny too long";
			return error(errorMessage.c_str());
		}
		if(theOffer.vchAliasPeg.size() > MAX_GUID_LENGTH)
		{
			errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 14 - Offer alias peg too long";
			return error(errorMessage.c_str());
		}
		if(theOffer.vchGeoLocation.size() > MAX_NAME_LENGTH)
		{
			errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 15 - Offer geolocation too long";
			return error(errorMessage.c_str());
		}
		if(theOffer.offerLinks.size() > 0)
		{
			errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 16 - Offer links are not allowed in the transaction data";
			return error(errorMessage.c_str());
		}
		if(theOffer.linkWhitelist.entries.size() > 1)
		{
			errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 17 - Offer has too many affiliate entries, only one allowed per transaction";
			return error(errorMessage.c_str());
		}
		if(!theOffer.vchOffer.empty() && theOffer.vchOffer != vvchArgs[0])
		{
			errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 18 - Offer guid in the data output does not match the guid in the transaction";
			return error(errorMessage.c_str());
		}
		if (vvchArgs[0].size() > MAX_GUID_LENGTH)
		{
			errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 19 - Offer guid too long";
			return error(errorMessage.c_str());
		}

		if(stringFromVch(theOffer.sCurrencyCode) != "BTC" && theOffer.bOnlyAcceptBTC)
		{
			errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 20 - Offer that only accepts BTC must have BTC specified as its currency";
			return error(errorMessage.c_str());
		}
		switch (op) {
		case OP_OFFER_ACTIVATE:
			if(!theOffer.accept.IsNull())
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 46 - Cannot have accept information on offer activation";
				return error(errorMessage.c_str());
			}
			if (!theOffer.vchCert.empty() && !IsCertOp(prevCertOp))
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 21 - You must own the certificate you wish to sell";
				return error(errorMessage.c_str());
			}
			if (IsCertOp(prevCertOp) && !theOffer.vchCert.empty() && theOffer.vchCert != vvchPrevCertArgs[0])
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 22 - Cert input and offer cert guid mismatch";
				return error(errorMessage.c_str());
			}
			if ( theOffer.vchOffer != vvchArgs[0])
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 23 - Offer input and offer guid mismatch";
				return error(errorMessage.c_str());
			}	
			if(!IsAliasOp(prevAliasOp) || theOffer.vchAlias != vvchPrevAliasArgs[0])
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 25 - Alias input mismatch";
				return error(errorMessage.c_str());
			}				
			if(!theOffer.vchLinkOffer.empty())
			{
				if(theOffer.nCommission > 255)
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 26 - Commission must be less than 256";
					return error(errorMessage.c_str());
				}
				if(theOffer.nCommission <= 0)
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 27 - Commission must be greator than 0";
					return error(errorMessage.c_str());
				}
				if(theOffer.bOnlyAcceptBTC)
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 28 - Linked offer cannot accept BTC only";
					return error(errorMessage.c_str());
				}
			}
			else
			{
				if(theOffer.sCategory.size() < 1)
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 32 - Offer category cannot be empty";
					return error(errorMessage.c_str());
				}
				if(theOffer.sTitle.size() < 1)
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 33 - Offer title cannot be empty";
					return error(errorMessage.c_str());
				}
			}
			if(theOffer.nQty < -1)
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 29 - Quantity must be greator than or equal to -1";
				return error(errorMessage.c_str());
			}
			if(!theOffer.vchCert.empty() && theOffer.nQty != 1)
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 30 - Quantity must be 1 for a digital offer";
				return error(errorMessage.c_str());
			}
			if(theOffer.nPrice <= 0)
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 31 - Offer price must be greater than 0";
				return error(errorMessage.c_str());
			}
			if(theOffer.bOnlyAcceptBTC && !theOffer.vchCert.empty())
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 34 - Cannot sell a digital offer accepting only Bitcoins";
				return error(errorMessage.c_str());
			}
			if(theOffer.bOnlyAcceptBTC && stringFromVch(theOffer.sCurrencyCode) != "BTC")
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 35 - Can only accept Bitcoins for offers that set their currency to BTC";
				return error(errorMessage.c_str());
			}

			break;
		case OP_OFFER_UPDATE:
			if(!theOffer.accept.IsNull())
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 47 - Cannot have accept information on offer update";
				return error(errorMessage.c_str());
			}
			if (!IsOfferOp(prevOp) )
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 36 - Offerupdate previous op is invalid";
				return error(errorMessage.c_str());
			}
			if(prevOp == OP_OFFER_ACCEPT)
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 37 - Cannot use offeraccept as input to an update";
				return error(errorMessage.c_str());
			}
			if (vvchPrevArgs[0] != vvchArgs[0])
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 38 - Offerupdate offer mismatch";
				return error(errorMessage.c_str());
			}
			if (IsCertOp(prevCertOp) && theOffer.vchCert != vvchPrevCertArgs[0])
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 39 - Cert input and offer cert guid mismatch";
				return error(errorMessage.c_str());
			}
			if (!theOffer.vchCert.empty() && !IsCertOp(prevCertOp))
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 40 - You must own the cert offer you wish to update";
				return error(errorMessage.c_str());
			}
			if ( theOffer.vchOffer != vvchArgs[0])
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 41 - Offer input and offer guid mismatch";
				return error(errorMessage.c_str());
			}
			if(!theOffer.linkWhitelist.IsNull() && (theOffer.linkWhitelist.entries[0].nDiscountPct < -99 || theOffer.linkWhitelist.entries[0].nDiscountPct > 99) && theOffer.linkWhitelist.entries[0].nDiscountPct != 127)
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 42 - Invalid discount amount, must be within -99 and 99 or 127 if removing an affiliate";
				return error(errorMessage.c_str());
			}

			if(theOffer.nQty < -1)
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 43 - Quantity must be greator than or equal to -1";
				return error(errorMessage.c_str());
			}
			if(!theOffer.vchCert.empty() && theOffer.nQty != 1)
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 44 - Quantity must be 1 for a digital offer";
				return error(errorMessage.c_str());
			}
			if(theOffer.nPrice <= 0)
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 45 - Offer price must be greater than 0";
				return error(errorMessage.c_str());
			}
			if(theOffer.bOnlyAcceptBTC && !theOffer.vchCert.empty())
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 48 - Cannot sell a certificate accepting only Bitcoins";
				return error(errorMessage.c_str());
			}
			if(theOffer.bOnlyAcceptBTC && stringFromVch(theOffer.sCurrencyCode) != "BTC")
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 49 - Can only accept Bitcoins for offer's that set their currency to BTC";
				return error(errorMessage.c_str());
			}
			break;
		case OP_OFFER_ACCEPT:
			theOfferAccept = theOffer.accept;
			if(!theOfferAccept.feedback.IsNull())
			{
				if(prevOp != OP_OFFER_ACCEPT)
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 50 - Must use offeraccept as input to an accept feedback";
					return error(errorMessage.c_str());
				}
				if (vvchPrevArgs[1] != vvchArgs[1])
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 51 - Offeraccept feedback mismatch";
					return error(errorMessage.c_str());
				}
				if(theOfferAccept.feedback.vchFeedback.empty())
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 52 - Cannot leave empty feedback";
					return error(errorMessage.c_str());
				}
				break;
			}

			if (theOfferAccept.IsNull())
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 53 - Offeraccept object cannot be empty";
				return error(errorMessage.c_str());
			}
			if (!IsValidAliasName(theOfferAccept.vchBuyerAlias))
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 119 - Invalid offer buyer alias";
				return error(errorMessage.c_str());
			}
			if (IsOfferOp(prevOp) && !theOfferAccept.feedback.IsNull() && vvchPrevArgs[1] != theOfferAccept.vchLinkAccept && vvchPrevArgs[0] != theOfferAccept.vchLinkOffer)
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 54 - Prev offer input and link accept guid mismatch";
				return error(errorMessage.c_str());
			}
			if (!IsOfferOp(prevOp) && !theOfferAccept.vchLinkAccept.empty() && !theOfferAccept.vchLinkOffer.empty())
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 54a - Prev offer input missing";
				return error(errorMessage.c_str());
			}
			if (IsEscrowOp(prevEscrowOp) && !theOfferAccept.txBTCId.IsNull())
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 55 - Cannot use BTC for escrow transactions";
				return error(errorMessage.c_str());
			}
			if (IsEscrowOp(prevEscrowOp) && theOfferAccept.vchEscrow != vvchPrevEscrowArgs[0])
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 56 - Escrow guid mismatch";
				return error(errorMessage.c_str());
			}
			if (!IsEscrowOp(prevEscrowOp) && !theOfferAccept.vchEscrow.empty())
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 56a - Escrow input is missing";
				return error(errorMessage.c_str());
			}
			if (IsAliasOp(prevAliasOp) && theOffer.vchLinkAlias != vvchPrevAliasArgs[0])
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 57 - Whitelist alias guid mismatch";
				return error(errorMessage.c_str());
			}
			if (!IsAliasOp(prevAliasOp) && !theOffer.vchLinkAlias.empty())
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 57a - Whitelist alias input missing";
				return error(errorMessage.c_str());
			}
			if (vvchArgs[1].size() > MAX_GUID_LENGTH)
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 58 - Offeraccept transaction with guid too big";
				return error(errorMessage.c_str());
			}
			if(prevOp == OP_OFFER_ACCEPT)
			{
				if(!theOfferAccept.txBTCId.IsNull())
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 59 - Cannot accept a linked offer with BTC";
					return error(errorMessage.c_str());
				}
			}


			if (theOfferAccept.vchAcceptRand.size() > MAX_GUID_LENGTH)
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 60 - Offer accept hex guid too long";
				return error(errorMessage.c_str());
			}
			if (theOfferAccept.vchLinkAccept.size() > MAX_GUID_LENGTH)
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 61 - Offer link accept hex guid too long";
				return error(errorMessage.c_str());
			}
			if (theOfferAccept.vchLinkOffer.size() > MAX_GUID_LENGTH)
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 62 - Offer link hex guid too long";
				return error(errorMessage.c_str());
			}
			if (theOfferAccept.vchMessage.size() > MAX_ENCRYPTED_VALUE_LENGTH)
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 63 - Message field too big";
				return error(errorMessage.c_str());
			}
			if (IsEscrowOp(prevEscrowOp) && IsOfferOp(prevOp))
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 64 - Offer accept cannot attach both escrow and offer inputs at the same time";
				return error(errorMessage.c_str());
			}
			if (theOffer.vchOffer != vvchArgs[0])
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 65 - Offer input and offer guid mismatch";
				return error(errorMessage.c_str());
			}

			break;

		default:
			errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 66 - Offer transaction has unknown op";
			return error(errorMessage.c_str());
		}
	}
	

	if (!fJustCheck ) {
		COffer serializedOffer;
		if(op != OP_OFFER_ACTIVATE) {
			// save serialized offer for later use
			serializedOffer = theOffer;
			CTransaction offerTx;
			// load the offer data from the DB
		
			if(!GetTxAndVtxOfOffer(vvchArgs[0], theOffer, offerTx, vtxPos))	
			{		
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 67 - Failed to read from offer DB";
				return true;				
			}						
		}
		// If update, we make the serialized offer the master
		// but first we assign fields from the DB since
		// they are not shipped in an update txn to keep size down
		if(op == OP_OFFER_UPDATE) {
			if(!theOffer.vchLinkOffer.empty())
			{
				if(serializedOffer.bOnlyAcceptBTC)
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 68 - Linked offer cannot accept BTC only";
					serializedOffer.bOnlyAcceptBTC = false;
				}
				
			}
			// if we are selling a cert ensure it exists and pubkey's match (to ensure it doesnt get transferred prior to accepting by user)
			if(!theOffer.vchCert.empty())
			{
				if(!theOffer.vchLinkOffer.empty())
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 69 - Cannot sell a digital offer as a linked offer";
					theOffer.vchCert.clear();	
				}
				else
				{
					CTransaction txCert;
					if (GetTxOfCert( theOffer.vchCert, theCert, txCert))
					{
						// if selling a cert, offers pubkey must match certs pubkey
						theOffer.vchAlias = theCert.vchAlias; 
					}
					else
					{
						errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 70 - Trying to update an offer with an expired certificate";
						theOffer.vchCert.clear();	
					}
				}
			}

			serializedOffer.offerLinks = theOffer.offerLinks;
			serializedOffer.vchLinkOffer = theOffer.vchLinkOffer;
			serializedOffer.vchOffer = theOffer.vchOffer;
			// cannot edit safety level
			serializedOffer.safetyLevel = theOffer.safetyLevel;
			// can't edit the alias of this offer
			serializedOffer.vchAlias = theOffer.vchAlias;
			serializedOffer.accept.SetNull();
			theOffer = serializedOffer;
			if(!vtxPos.empty())
			{
				const COffer& dbOffer = vtxPos.back();
				// if updating whitelist, we dont allow updating any offer details
				if(theOffer.linkWhitelist.entries.size() > 0)
					theOffer = dbOffer;
				else
				{
					// whitelist must be preserved in serialOffer and db offer must have the latest in the db for whitelists
					theOffer.linkWhitelist.entries = dbOffer.linkWhitelist.entries;
					// some fields are only updated if they are not empty to limit txn size, rpc sends em as empty if we arent changing them
					if(serializedOffer.sCategory.empty())
						theOffer.sCategory = dbOffer.sCategory;
					if(serializedOffer.sTitle.empty())
						theOffer.sTitle = dbOffer.sTitle;
					if(serializedOffer.sDescription.empty())
						theOffer.sDescription = dbOffer.sDescription;
					if(serializedOffer.vchGeoLocation.empty())
						theOffer.vchGeoLocation = dbOffer.vchGeoLocation;
					if(serializedOffer.vchAliasPeg.empty())
						theOffer.vchAliasPeg = dbOffer.vchAliasPeg;
					// user can't update safety level after creation
					theOffer.safetyLevel = dbOffer.safetyLevel;
					if(!GetTxOfAlias(theOffer.vchAlias, alias, aliasTx))
					{
						errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 71 - Alias expiry check for offer";
						theOffer = dbOffer;
					}
				}
			}
			// check for valid alias peg
			if(getCurrencyToSYSFromAlias(theOffer.vchAliasPeg, theOffer.sCurrencyCode, nRate, theOffer.nHeight, rateList,precision) != "")
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 72 - Could not find currency " + stringFromVch(theOffer.sCurrencyCode) + " in the " + stringFromVch(theOffer.vchAliasPeg) + " alias";
				return true;
			}			
		}
		else if(op == OP_OFFER_ACTIVATE)
		{
			if(!GetTxOfAlias(theOffer.vchAlias, alias, aliasTx))
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 73 - Alias expiry check for offer" ;
				return true;	
			}
			if (pofferdb->ExistsOffer(vvchArgs[0]))
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 74 - Offer already exists";
				return true;
			}
			// by default offers are private on new, need to update it out of privacy
			theOffer.bPrivate = true;
			// if we are selling a cert ensure it exists and pubkey's match (to ensure it doesnt get transferred prior to accepting by user)
			if(!theOffer.vchCert.empty())
			{
				CTransaction txCert;
				if (GetTxOfCert( theOffer.vchCert, theCert, txCert))
				{
					// if selling a cert, offers pubkey must match certs pubkey
					theOffer.vchAlias = theCert.vchAlias;
				}
				else
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 75 - Creating an offer with a cert that does not exist";
					theOffer.vchCert.clear();
				}			
			}
			// if this is a linked offer activate, then add it to the parent offerLinks list
			if(!theOffer.vchLinkOffer.empty())
			{
				CTransaction txOffer;
				vector<COffer> myVtxPos;
				if (GetTxAndVtxOfOffer( theOffer.vchLinkOffer, linkOffer, txOffer, myVtxPos))
				{					
					// if alias input is given then make sure it exists in the root offer affiliate list if root offer is in exclusive mode
					if (linkOffer.linkWhitelist.bExclusiveResell)
					{
						if(!linkOffer.linkWhitelist.GetLinkEntryByHash(theOffer.vchAlias, entry))
						{
							errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 76 - Cannot find this alias in the parent offer affiliate list";
							theOffer.vchLinkOffer.clear();	
						}
						else if(theOffer.nCommission <= -entry.nDiscountPct)
						{
							errorMessage = strprintf("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 76a - You cannot re-sell at a lower price than the discount you received as an affiliate (current discount received: %d%%)", entry.nDiscountPct));
							theOffer.vchLinkOffer.clear();
						}
						else if (!GetTxOfAlias(theOffer.vchAlias, theAlias, aliasTx))
						{
							errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 78 - Cannot find alias used to create linked offer. Perhaps it is expired";
							theOffer.vchLinkOffer.clear();
						}
					}
					else if (!linkOffer.vchLinkOffer.empty())
					{
						errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 77 - Cannot link to an offer that is already linked to another offer";
						theOffer.vchLinkOffer.clear();	
					}
					else if(linkOffer.bOnlyAcceptBTC)
					{
						errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 79 - Cannot link to an offer that only accepts Bitcoins as payment";
						theOffer.vchLinkOffer.clear();	
					}
					if(!theOffer.vchLinkOffer.empty())
					{
						// max links are 100 per offer
						if(linkOffer.offerLinks.size() < 100)
						{
							// if creating a linked offer we set some mandatory fields to the parent
							theOffer.nQty = linkOffer.nQty;
							theOffer.linkWhitelist.bExclusiveResell = true;
							theOffer.sCurrencyCode = linkOffer.sCurrencyCode;
							theOffer.vchCert = linkOffer.vchCert;
							theOffer.vchAliasPeg = linkOffer.vchAliasPeg;
							theOffer.sCategory = linkOffer.sCategory;
							theOffer.sTitle = linkOffer.sTitle;
							linkOffer.offerLinks.push_back(vvchArgs[0]);
							linkOffer.PutToOfferList(myVtxPos);
							// write parent offer
					
							if (!dontaddtodb && !pofferdb->WriteOffer(theOffer.vchLinkOffer, myVtxPos))
							{
								errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 80 - Failed to write to offer link to DB";
								return error(errorMessage.c_str());
							}
						}
						else
						{
							errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 80 - Parent offer affiliate table exceeded 100 entries";
							theOffer.vchLinkOffer.clear();
						}					
					}
				}
				else
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 81 - Linked offer is expired";
					theOffer.vchLinkOffer.clear();	
				}
			}
			else
			{
				// check for valid alias peg
				if(getCurrencyToSYSFromAlias(theOffer.vchAliasPeg, theOffer.sCurrencyCode, nRate, theOffer.nHeight, rateList,precision) != "")
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 82 - Could not find currency " + stringFromVch(theOffer.sCurrencyCode) + " in the " + stringFromVch(theOffer.vchAliasPeg) + " alias";
					return true;
				}
			}
		}
		else if (op == OP_OFFER_ACCEPT) {
			theOfferAccept = serializedOffer.accept;
			if(!GetTxOfAlias(theOfferAccept.vchBuyerAlias, alias, aliasTx))
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 83 - Alias expiry check for offer";
				return true;	
			}
			// trying to purchase a cert
			if(!theOffer.vchCert.empty())
			{
				CTransaction txCert;
				if (!GetTxOfCert( theOffer.vchCert, theCert, txCert))
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 84 - Cannot sell an expired certificate";
					return true;
				}
				else if(!theOfferAccept.txBTCId.IsNull())
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 85 - Cannot purchase certificates with Bitcoins";
					return true;
				}
				else if(theOffer.vchLinkOffer.empty() && theCert.vchAlias != theOffer.vchAlias)
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 86 - Cannot purchase this offer because the certificate has been transferred or it is linked to another offer";
					return true;
				}
			}
			else if (theOfferAccept.vchMessage.size() <= 0)
			{
				errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 87 - Offer payment message cannot be empty";
				return true;
			}
					
			if(vvchArgs[2] == vchFromString("1"))
			{
				if(fDebug)
					LogPrintf( "SYSCOIN_OFFER_CONSENSUS_ERROR Buyer special accept output... skipping\n");
				return true;
			}
			if(!theOfferAccept.feedback.IsNull())
			{
				if(!theOffer.vchLinkOffer.empty())
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 88 - Cannot leave feedback for linked offers";
					return true;
				}
				// ensure we don't add same feedback twice (feedback in db should be older than current height)
				if(theOfferAccept.feedback.nHeight < nHeight)
				{
					theOfferAccept.feedback.nHeight = nHeight;
					theOfferAccept.feedback.txHash = tx.GetHash();
				}
				else
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 89 - Feedback in db is newer than the current height";
					return true;
				}

				int numRatings;				
				int feedbackCount = FindFeedbackInAccept(vvchArgs[1], theOfferAccept.feedback.nFeedbackUser, vtxPos, numRatings);
				// has this user (nFeedbackUser) already rated? if so set desired rating to 0
				if(numRatings > 0)
					theOfferAccept.feedback.nRating = 0;
				if(feedbackCount >= 10)
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 90 - Cannot exceed 10 feedback entries for this user of this offer accept";
					return true;
				}
				HandleAcceptFeedback(theOfferAccept, theOffer);	
			
			}
			// if its not a special feedback output for the buyer then we decrease qty accordingly
			else
			{

				if(theOfferAccept.nQty <= 0)
					theOfferAccept.nQty = 1;
				// update qty if not an escrow accept (since that updates qty on escrow creation, and refunds qty on escrow refund)
				// also if this offer you are accepting is linked to another offer don't need to update qty (once the root accept is done this offer qty will be updated)
				if(theOffer.nQty != -1 && theOfferAccept.vchEscrow.empty() && theOffer.vchLinkOffer.empty())
				{
					if((theOfferAccept.nQty > theOffer.nQty))
					{
						errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 91 - Not enough quantity left in this offer for this purchase";
						return true;
					}				
					theOffer.nQty -= theOfferAccept.nQty;
					if(theOffer.nQty < 0)
						theOffer.nQty = 0;
					
				}
			
				if(theOffer.sCategory.size() > 0 && boost::algorithm::ends_with(stringFromVch(theOffer.sCategory), "wanted"))
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 92 - Cannot purchase a wanted offer";
					return true;
				}
				if(!theOffer.vchLinkOffer.empty())
				{

					if(!theOfferAccept.txBTCId.IsNull())
					{
						errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 93 - Cannot accept a linked offer by paying in Bitcoins";
						return true;
					}
					else if(!GetTxOfOffer( theOffer.vchLinkOffer, linkOffer, linkedTx))
					{
						errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 94 - Could not get linked offer";
						return true;
					}
					else if(!theOffer.vchCert.empty() && theCert.vchAlias != linkOffer.vchAlias)
					{
						errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 95 - Cannot purchase this linked offer because the certificate has been transferred or it is linked to another offer";
						return true;
					}
					else if (linkOffer.bOnlyAcceptBTC)
					{
						errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 96 - Linked offer only accepts Bitcoins, linked offers currently only work with Syscoin payments";
						return true;
					}
					else if(!serializedOffer.vchLinkAlias.empty())
					{
						errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 97 - Purchase discounts can only be applied to non-linked offers";
						return true;
					}
					
					// make sure that linked offer exists in root offerlinks (offers that are linked to the root offer)
					else if(std::find(linkOffer.offerLinks.begin(), linkOffer.offerLinks.end(), vvchArgs[0]) == linkOffer.offerLinks.end())
					{
						errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 98 - This offer does not exist in the root offerLinks table";
						return true;
					}					
				}
				
				// find the payment from the tx outputs (make sure right amount of coins were paid for this offer accept), the payment amount found has to be exact	
				heightToCheckAgainst = theOfferAccept.nAcceptHeight;
				// if this is a linked offer accept, set the height to the first height so sys_rates price will match what it was at the time of the original accept
				// we assume previous tx still in mempool because it calls offeraccept within the checkinputs stage (not entering a block yet)
				if (!theOfferAccept.vchLinkAccept.empty())
				{				
					CTransaction acceptTx;
					COfferAccept theLinkedOfferAccept;
					COffer offer;
					bool skipFeedback = true;
					if (!GetTxOfOfferAccept(theOfferAccept.vchLinkOffer, theOfferAccept.vchLinkAccept, offer, theLinkedOfferAccept, acceptTx, skipFeedback))
					{
						errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 99 - Could not find a linked offer accept from mempool or disk";
						return true;
					}	
					// if this is an accept for a linked offer, the root offer is set to exclusive mode and reseller doesn't have an alias in the whitelist, you cannot accept this linked offer
					// serializedOffer.vchLinkAlias is validated below if its not empty
					if(serializedOffer.vchLinkAlias.empty() && theOffer.linkWhitelist.bExclusiveResell)
					{
						errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 100 - Cannot pay for this linked offer because you don't own an alias from its affiliate list. Root offer is set to exclusive mode";
						return true;
					}	
					theOfferAccept.nQty = theLinkedOfferAccept.nQty;
					heightToCheckAgainst = theLinkedOfferAccept.nAcceptHeight;
					linkAccept = true;
					if(theOfferAccept.vchBuyerAlias != theLinkedOfferAccept.vchBuyerAlias)
					{
						errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 101 - Linked accept buyer must match the buyer of the reselling offer";
						return true;
					}
				}
				// if this accept was done via an escrow release, we get the height from escrow and use that to lookup the price at the time
				if(!theOffer.accept.vchEscrow.empty())
				{		
					vector<CEscrow> escrowVtxPos;
					CEscrow escrow;
					CTransaction escrowTx;
					if (GetTxAndVtxOfEscrow( theOffer.accept.vchEscrow, escrow, escrowTx, escrowVtxPos))
					{
						// we want the initial funding escrow transaction height as when to calculate this offer accept price
						CEscrow fundingEscrow = escrowVtxPos.front();
						if(fundingEscrow.vchOffer != vvchArgs[0])
						{
							errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 102 - Escrow guid does not match the guid of the offer you are accepting";
							return true;
						}
						// override height if funding escrow was before the accept that the buyer did, to index into sysrates
						if(heightToCheckAgainst > fundingEscrow.nHeight || !linkAccept){
							escrowAccept = true;
							heightToCheckAgainst = fundingEscrow.nHeight;
						}
					}
					else
					{
						errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 103 - Cannot find escrow linked to this offer accept";
						return true;
					}
				}
				// the height really shouldnt change cause we set it correctly in offeraccept
				if(heightToCheckAgainst != theOfferAccept.nAcceptHeight)
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 104 - Height mismatch with calculated height and accept height";
					return true;
				}
	
				myPriceOffer.nHeight = heightToCheckAgainst;
				myPriceOffer.GetOfferFromList(vtxPos);
				// if the buyer uses an alias for a discount or a exclusive whitelist buy, then get the guid
				if(!serializedOffer.vchLinkAlias.empty())
				{
					if (!GetTxOfAlias(serializedOffer.vchLinkAlias, theAlias, aliasTx))
					{
						errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 105 - Cannot find the alias you are trying to use for an offer discount. Perhaps it is expired";
						return true;
					}
					// try to get the whitelist entry here from the sellers whitelist, apply the discount with GetPrice()
					myPriceOffer.linkWhitelist.GetLinkEntryByHash(serializedOffer.vchLinkAlias, entry);
					if(entry.IsNull())
					{
						errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 106 - Cannot find the alias entry in the offer's affiliate list";
						return true;
					}
				}
				
				if(!theOfferAccept.txBTCId.IsNull() && stringFromVch(myPriceOffer.sCurrencyCode) != "BTC")
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 107 - Cannot pay for offer in bitcoins if its currency is not set to BTC";
					return true;
				}
				// check that user pays enough in syscoin if the currency of the offer is not directbtc purchase
				if(theOfferAccept.txBTCId.IsNull())
				{
		
					float priceAtTimeOfAccept = myPriceOffer.GetPrice(entry);
					if(priceAtTimeOfAccept != theOfferAccept.nPrice)
					{
						errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 108 - Offer accept does not specify the correct payment amount";
						return true;
					}

					int precision = 2;
					// lookup the price of the offer in syscoin based on pegged alias at the block # when accept/escrow was made
					CAmount nPrice = convertCurrencyCodeToSyscoin(myPriceOffer.vchAliasPeg, myPriceOffer.sCurrencyCode, priceAtTimeOfAccept, heightToCheckAgainst, precision)*theOfferAccept.nQty;
					if(!FindOfferAcceptPayment(tx, nPrice))
					{
						nPrice = convertCurrencyCodeToSyscoin(myPriceOffer.vchAliasPeg, myPriceOffer.sCurrencyCode, priceAtTimeOfAccept, heightToCheckAgainst+1, precision)*theOfferAccept.nQty;
						if(!FindOfferAcceptPayment(tx, nPrice))
						{
							nPrice = convertCurrencyCodeToSyscoin(myPriceOffer.vchAliasPeg, myPriceOffer.sCurrencyCode, priceAtTimeOfAccept, heightToCheckAgainst-1, precision)*theOfferAccept.nQty;
							if(!FindOfferAcceptPayment(tx, nPrice))
							{
								errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 109 - Offer accept does not pay enough according to the offer price";
								return true;
							}
						}
					}												
				}	
				// if not escrow check qty to see if enough, escrow creation already deducts qty
				if(!theOffer.accept.vchEscrow.empty())
				{
					if(theOfferAccept.nQty <= 0 || (theOffer.nQty != -1 && theOfferAccept.nQty > theOffer.nQty) || (!linkOffer.IsNull() && theOfferAccept.nQty > linkOffer.nQty && linkOffer.nQty != -1))
					{
						errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 110 - Offer does not have enough quantity left for this purchase";
						return true;
					}					
				}
			}

			theOfferAccept.nHeight = nHeight;
			theOfferAccept.vchAcceptRand = vvchArgs[1];
			theOfferAccept.txHash = tx.GetHash();
			theOffer.accept = theOfferAccept;
		}
		

		if(op == OP_OFFER_UPDATE)
		{
			// ensure the accept is null as this should just have the offer information and no accept information
			theOffer.accept.SetNull();
			// if the txn whitelist entry exists (meaning we want to remove or add)
			if(serializedOffer.linkWhitelist.entries.size() == 1)
			{
				// special case we use to remove all entries
				if(serializedOffer.linkWhitelist.entries[0].nDiscountPct == 127)
				{
					if(theOffer.linkWhitelist.IsNull())
					{
						errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 111 - Whitelist is already empty";					
					}
					else
						theOffer.linkWhitelist.SetNull();
				}
				// the stored offer has this entry meaning we want to remove this entry
				else if(theOffer.linkWhitelist.GetLinkEntryByHash(serializedOffer.linkWhitelist.entries[0].aliasLinkVchRand, entry))
				{
					theOffer.linkWhitelist.RemoveWhitelistEntry(serializedOffer.linkWhitelist.entries[0].aliasLinkVchRand);
				}
				// we want to add it to the whitelist
				else
				{
					if(!serializedOffer.linkWhitelist.entries[0].aliasLinkVchRand.empty())
					{
						if (GetTxOfAlias(serializedOffer.linkWhitelist.entries[0].aliasLinkVchRand, theAlias, aliasTx))
						{
							theOffer.linkWhitelist.PutWhitelistEntry(serializedOffer.linkWhitelist.entries[0]);
						}
						else
						{
							errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 112 - Cannot find the alias you are trying to offer affiliate list. Perhaps it is expired";					
						}
					}
				}

			}
			// if this offer is linked to a parent update it with parent information
			if(!theOffer.vchLinkOffer.empty())
			{
				CTransaction txOffer;
				vector<COffer> myVtxPos;
				if (GetTxAndVtxOfOffer( theOffer.vchLinkOffer, linkOffer, txOffer, myVtxPos))
				{
					theOffer.nQty = linkOffer.nQty;	
					theOffer.vchAliasPeg = linkOffer.vchAliasPeg;	
					theOffer.sCurrencyCode = linkOffer.sCurrencyCode;
					theOffer.vchCert = linkOffer.vchCert;
					theOffer.SetPrice(linkOffer.nPrice);				
				}
				else
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 113 - Linked offer is expired";		
					theOffer.vchLinkOffer.clear();
				}
			}
			else
			{
				// go through the linked offers, if any, and update the linked offer info based on the this info
				for(unsigned int i=0;i<theOffer.offerLinks.size();i++) {
					CTransaction txOffer;
					vector<COffer> myVtxPos;
					if (GetTxAndVtxOfOffer( theOffer.offerLinks[i], linkOffer, txOffer, myVtxPos))
					{

						linkOffer.nQty = theOffer.nQty;	
						linkOffer.vchAliasPeg = theOffer.vchAliasPeg;	
						linkOffer.sCurrencyCode = theOffer.sCurrencyCode;	
						linkOffer.SetPrice(theOffer.nPrice);
						linkOffer.vchCert = theOffer.vchCert;
						linkOffer.PutToOfferList(myVtxPos);
						// write offer
					
						if (!dontaddtodb && !pofferdb->WriteOffer(theOffer.offerLinks[i], myVtxPos))
						{
							errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 114 - Failed to write to offer link to DB";		
							return error(errorMessage.c_str());		
						}
					
					}
				}			
			}
		}
		theOffer.nHeight = nHeight;
		theOffer.txHash = tx.GetHash();
		theOffer.PutToOfferList(vtxPos);
		// write offer
	
		if (!dontaddtodb && !pofferdb->WriteOffer(vvchArgs[0], vtxPos))
		{
			errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 115 - Failed to write to offer DB";		
			return error(errorMessage.c_str());
		}
		
		if(op == OP_OFFER_ACCEPT)
		{
			COffer linkOffer;
 			if(theOffer.vchLinkOffer.empty())
			{
				// go through the linked offers, if any, and update the linked offer qty based on the this qty
				for(unsigned int i=0;i<theOffer.offerLinks.size();i++) {
					CTransaction txOffer;
					vector<COffer> myVtxPos;
					if (GetTxAndVtxOfOffer( theOffer.offerLinks[i], linkOffer, txOffer, myVtxPos))
					{						
						linkOffer.nQty = theOffer.nQty;	
						linkOffer.PutToOfferList(myVtxPos);
						// write offer
					
						if (!dontaddtodb && !pofferdb->WriteOffer(theOffer.offerLinks[i], myVtxPos))
						{
							errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 116 - Failed to write to offer link to DB";		
							return error(errorMessage.c_str());
						}						
					}
				}
			}
 			
			// if its my offer and its linked and its not a special feedback output for the buyer
			if (!dontaddtodb && pwalletMain && !theOffer.vchLinkOffer.empty() && IsSyscoinTxMine(tx, "offer"))
			{	
				// theOffer.vchLinkOffer is the linked offer guid
				// theOffer is this reseller offer used to get pubkey to send to offeraccept as first parameter
				string strError = makeOfferLinkAcceptTX(theOfferAccept, vvchArgs[0], theOfferAccept.vchMessage, theOffer.vchLinkOffer, tx.GetHash().GetHex(), theOffer, alias, block);
				if(strError != "")		
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 117 - Offer link accept failure: " + strError;	
				}
				
			} 
			// only if we are the root offer owner do we even consider xfering a cert					
 			// purchased a cert so xfer it
			// also can't auto xfer offer paid in btc, need to do manually
 			if(!dontaddtodb && pwalletMain && theOfferAccept.txBTCId.IsNull() && IsSyscoinTxMine(tx, "offer") && !theOffer.vchCert.empty() && theOffer.vchLinkOffer.empty())
 			{
 				string strError = makeTransferCertTX(theOffer, theOfferAccept);
 				if(strError != "")
				{
					errorMessage = "SYSCOIN_OFFER_CONSENSUS_ERROR: ERRCODE: 118 - Transfer certificate failure: " + strError;	
				}
 			}
		}
		// debug
		if (fDebug)
			LogPrintf( "CONNECTED OFFER: op=%s offer=%s title=%s qty=%u hash=%s height=%d\n",
				offerFromOp(op).c_str(),
				stringFromVch(vvchArgs[0]).c_str(),
				stringFromVch(theOffer.sTitle).c_str(),
				theOffer.nQty,
				tx.GetHash().ToString().c_str(), 
				nHeight);
	}
	return true;
}

UniValue offernew(const UniValue& params, bool fHelp) {
	if (fHelp || params.size() < 7 || params.size() > 13)
		throw runtime_error(
		"offernew <aliaspeg> <alias> <category> <title> <quantity> <price> <description> <currency> [cert. guid] [exclusive resell=1] [accept btc only=0] [geolocation=''] [safe search=Yes]\n"
						"<aliaspeg> Alias peg you wish to use, leave blank to use sys_rates.\n"	
						"<alias> An alias you own.\n"
						"<category> category, 255 chars max.\n"
						"<title> title, 255 chars max.\n"
						"<quantity> quantity, > 0 or -1 for infinite\n"
						"<price> price in <currency>, > 0\n"
						"<description> description, 1 KB max.\n"
						"<currency> The currency code that you want your offer to be in ie: USD.\n"
						"<cert. guid> Set this to the guid of a certificate you wish to sell\n"
						"<exclusive resell> set to 1 if you only want those who control the affiliate's who are able to resell this offer via offerlink. Defaults to 1.\n"
						"<accept btc only> set to 1 if you only want accept Bitcoins for payment and your currency is set to BTC, note you cannot resell or sell a cert in this mode. Defaults to 0.\n"
						"<geolocation> set to your geolocation. Defaults to empty. \n"
						"<safe search> set to No if this offer should only show in the search when safe search is not selected. Defaults to Yes (offer shows with or without safe search selected in search lists).\n"
						+ HelpRequiringPassphrase());
	// gather inputs
	string baSig;
	float nPrice;
	bool bExclusiveResell = true;
	vector<unsigned char> vchAliasPeg = vchFromValue(params[0]);
	vector<unsigned char> vchAlias = vchFromValue(params[1]);

	CTransaction aliastx;
	CAliasIndex alias;
	const CWalletTx *wtxAliasIn = NULL;
	if (!GetTxOfAlias(vchAlias, alias, aliastx, true))
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 500 - Could not find an alias with this name");
    if(!IsSyscoinTxMine(aliastx, "alias")) {
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 501 - This alias is not yours.");
    }
	wtxAliasIn = pwalletMain->GetWalletTx(aliastx.GetHash());
	if (wtxAliasIn == NULL)
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 502 - This alias is not in your wallet");
	if (ExistsInMempool(vchAlias, OP_ALIAS_ACTIVATE) || ExistsInMempool(vchAlias, OP_ALIAS_UPDATE)) {
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 502a - There are pending operations on that alias");
	}
	vector<unsigned char> vchCat = vchFromValue(params[2]);
	vector<unsigned char> vchTitle = vchFromValue(params[3]);
	vector<unsigned char> vchCurrency = vchFromValue(params[7]);
	vector<unsigned char> vchDesc;
	vector<unsigned char> vchCert;
	bool bOnlyAcceptBTC = false;
	int nQty;

	try {
		nQty = atoi(params[4].get_str());
	} catch (std::exception &e) {
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 503 - Invalid quantity value, must be less than 4294967296 and greater than or equal to -1");
	}
	nPrice = atof(params[5].get_str().c_str());
	vchDesc = vchFromValue(params[6]);
	CScript scriptPubKeyOrig, scriptPubKeyCertOrig;
	CScript scriptPubKey, scriptPubKeyCert;
	const CWalletTx *wtxCertIn = NULL;
	CCert theCert;
	if(params.size() >= 9)
	{
		
		vchCert = vchFromValue(params[8]);
		if(vchCert == vchFromString("nocert"))
			vchCert.clear();
		CTransaction txCert;
		
		// make sure this cert is still valid
		if (GetTxOfCert( vchCert, theCert, txCert, true))
		{
      		// check for existing cert 's
			if (ExistsInMempool(vchCert, OP_CERT_UPDATE) || ExistsInMempool(vchCert, OP_CERT_TRANSFER)) {
				throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 504 - There are pending operations on that cert");
			}
			// make sure its in your wallet (you control this cert)		
			if (IsSyscoinTxMine(txCert, "cert")) 
			{
				CAliasIndex certAlias;
				CTransaction certaliastx;
				if (GetTxOfAlias( theCert.vchAlias, certAlias, certaliastx, true))
				{
					wtxCertIn = pwalletMain->GetWalletTx(txCert.GetHash());
					CPubKey currentCertKey(certAlias.vchPubKey);
					scriptPubKeyCertOrig = GetScriptForDestination(currentCertKey.GetID());
					scriptPubKeyCert << CScript::EncodeOP_N(OP_CERT_UPDATE) << vchCert << vchFromString("") << OP_2DROP << OP_DROP;
					scriptPubKeyCert += scriptPubKeyCertOrig;
				}
			}

		}
	}

	if(params.size() >= 10)
	{
		bExclusiveResell = atoi(params[9].get_str().c_str()) == 1? true: false;
	}
	if(params.size() >= 11)
	{
		bOnlyAcceptBTC = atoi(params[10].get_str().c_str()) == 1? true: false;

	}	
	string strGeoLocation = "";
	if(params.size() >= 12)
	{
		strGeoLocation = params[11].get_str();
	}
	string strSafeSearch = "Yes";
	if(params.size() >= 13)
	{
		strSafeSearch = params[12].get_str();
	}

	CAmount nRate;
	vector<string> rateList;
	int precision;
	if(getCurrencyToSYSFromAlias(vchAliasPeg, vchCurrency, nRate, chainActive.Tip()->nHeight, rateList,precision) != "")
	{
		string err = strprintf("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 505 - Could not find currency %s in the %s alias!\n", stringFromVch(vchCurrency), stringFromVch(vchAliasPeg));
		throw runtime_error(err.c_str());
	}
	double minPrice = pow(10.0,-precision);
	double price = nPrice;
	if(price < minPrice)
		price = minPrice; 
	// this is a syscoin transaction
	CWalletTx wtx;

	// generate rand identifier
	int64_t rand = GetRand(std::numeric_limits<int64_t>::max());
	vector<unsigned char> vchRand = CScriptNum(rand).getvch();
	vector<unsigned char> vchOffer = vchFromString(HexStr(vchRand));

	EnsureWalletIsUnlocked();


	// unserialize offer from txn, serialize back
	// build offer
	COffer newOffer;
	newOffer.vchAlias = alias.vchAlias;
	newOffer.vchOffer = vchOffer;
	newOffer.sCategory = vchCat;
	newOffer.sTitle = vchTitle;
	newOffer.sDescription = vchDesc;
	newOffer.nQty = nQty;
	newOffer.nHeight = chainActive.Tip()->nHeight;
	newOffer.SetPrice(price);
	newOffer.vchCert = vchCert;
	newOffer.linkWhitelist.bExclusiveResell = bExclusiveResell;
	newOffer.sCurrencyCode = vchCurrency;
	newOffer.bPrivate = true;
	newOffer.bOnlyAcceptBTC = bOnlyAcceptBTC;
	newOffer.vchAliasPeg = vchAliasPeg;
	newOffer.safetyLevel = 0;
	newOffer.safeSearch = strSafeSearch == "Yes"? true: false;
	newOffer.vchGeoLocation = vchFromString(strGeoLocation);

	const vector<unsigned char> &data = newOffer.Serialize();
    uint256 hash = Hash(data.begin(), data.end());
 	vector<unsigned char> vchHash = CScriptNum(hash.GetCheapHash()).getvch();
    vector<unsigned char> vchHashOffer = vchFromValue(HexStr(vchHash));
	CPubKey currentOfferKey(alias.vchPubKey);
	scriptPubKeyOrig= GetScriptForDestination(currentOfferKey.GetID());
	scriptPubKey << CScript::EncodeOP_N(OP_OFFER_ACTIVATE) << vchOffer << vchHashOffer << OP_2DROP << OP_DROP;
	scriptPubKey += scriptPubKeyOrig;

	CScript scriptPubKeyAlias;
	scriptPubKeyAlias << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << vchAlias << alias.vchGUID << vchFromString("") << OP_2DROP << OP_2DROP;
	scriptPubKeyAlias += scriptPubKeyOrig;

	vector<CRecipient> vecSend;
	CRecipient recipient;
	CreateRecipient(scriptPubKey, recipient);
	vecSend.push_back(recipient);
	CRecipient certRecipient;
	CreateRecipient(scriptPubKeyCert, certRecipient);
	CRecipient aliasRecipient;
	CreateRecipient(scriptPubKeyAlias, aliasRecipient);
	vecSend.push_back(aliasRecipient);
	// if we use a cert as input to this offer tx, we need another utxo for further cert transactions on this cert, so we create one here
	if(wtxCertIn != NULL)
		vecSend.push_back(certRecipient);

	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);
	const CWalletTx * wtxInOffer=NULL;
	const CWalletTx * wtxInEscrow=NULL;
	SendMoneySyscoin(vecSend, recipient.nAmount+fee.nAmount, false, wtx, wtxInOffer, wtxCertIn, wtxAliasIn, wtxInEscrow);
	UniValue res(UniValue::VARR);
	res.push_back(wtx.GetHash().GetHex());
	res.push_back(HexStr(vchRand));
	return res;
}

UniValue offerlink(const UniValue& params, bool fHelp) {
	if (fHelp || params.size() < 3 || params.size() > 4)
		throw runtime_error(
		"offerlink <alias> <guid> <commission> [description]\n"
						"<alias> An alias you own.\n"
						"<guid> offer guid that you are linking to\n"
						"<commission> percentage of profit desired over original offer price, > 0, ie: 5 for 5%\n"
						"<description> description, 1 KB max. Defaults to original description. Leave as '' to use default.\n"
						+ HelpRequiringPassphrase());
	// gather inputs
	string baSig;
	COfferLinkWhitelistEntry whiteListEntry;
	vector<unsigned char> vchAlias = vchFromValue(params[0]);


	CTransaction aliastx;
	CAliasIndex alias;
	const CWalletTx *wtxAliasIn = NULL;
	if (!GetTxOfAlias(vchAlias, alias, aliastx, true))
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 506 - Could not find an alias with this name");
    if(!IsSyscoinTxMine(aliastx, "alias")) {
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 507 - This alias is not yours");
    }
	wtxAliasIn = pwalletMain->GetWalletTx(aliastx.GetHash());
	if (wtxAliasIn == NULL)
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 508 - This alias is not in your wallet");
	if (ExistsInMempool(vchAlias, OP_ALIAS_ACTIVATE) || ExistsInMempool(vchAlias, OP_ALIAS_UPDATE)) {
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 508a - There are pending operations on that alias");
	}

	vector<unsigned char> vchLinkOffer = vchFromValue(params[1]);
	vector<unsigned char> vchDesc;
	// look for a transaction with this key
	CTransaction tx;
	COffer linkOffer;
	if (vchLinkOffer.empty() || !GetTxOfOffer( vchLinkOffer, linkOffer, tx, true))
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 509 - Could not find an offer with this guid");

	int commissionInteger = atoi(params[2].get_str().c_str());
	if(params.size() >= 4)
	{

		vchDesc = vchFromValue(params[3]);
		if(vchDesc.empty())
		{
			vchDesc = linkOffer.sDescription;
		}
	}
	else
	{
		vchDesc = linkOffer.sDescription;
	}	


	COfferLinkWhitelistEntry entry, foundEntry;
	CScript scriptPubKeyOrig;
	CScript scriptPubKey;

	
	// this is a syscoin transaction
	CWalletTx wtx;


	// generate rand identifier
	int64_t rand = GetRand(std::numeric_limits<int64_t>::max());
	vector<unsigned char> vchRand = CScriptNum(rand).getvch();
	vector<unsigned char> vchOffer = vchFromString(HexStr(vchRand));
	int precision = 2;
	// get precision
	convertCurrencyCodeToSyscoin(linkOffer.vchAliasPeg, linkOffer.sCurrencyCode, linkOffer.GetPrice(), chainActive.Tip()->nHeight, precision);
	double minPrice = pow(10.0,-precision);
	double price = linkOffer.GetPrice();
	if(price < minPrice)
		price = minPrice;

	EnsureWalletIsUnlocked();
	
	// unserialize offer from txn, serialize back
	// build offer
	COffer newOffer;
	newOffer.vchOffer = vchOffer;
	newOffer.vchAlias = alias.vchAlias;
	newOffer.sDescription = vchDesc;
	newOffer.SetPrice(price);
	newOffer.nCommission = commissionInteger;
	newOffer.vchLinkOffer = vchLinkOffer;
	newOffer.nHeight = chainActive.Tip()->nHeight;
	//create offeractivate txn keys

	const vector<unsigned char> &data = newOffer.Serialize();
    uint256 hash = Hash(data.begin(), data.end());
 	vector<unsigned char> vchHash = CScriptNum(hash.GetCheapHash()).getvch();
    vector<unsigned char> vchHashOffer = vchFromValue(HexStr(vchHash));
	CPubKey aliasKey(alias.vchPubKey);
	scriptPubKeyOrig = GetScriptForDestination(aliasKey.GetID());
	scriptPubKey << CScript::EncodeOP_N(OP_OFFER_ACTIVATE) << vchOffer << vchHashOffer << OP_2DROP << OP_DROP;
	scriptPubKey += scriptPubKeyOrig;

	CScript scriptPubKeyAlias;
	scriptPubKeyAlias << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << vchAlias << alias.vchGUID << vchFromString("") << OP_2DROP << OP_2DROP;
	scriptPubKeyAlias += scriptPubKeyOrig;

	string strError;

	vector<CRecipient> vecSend;
	CRecipient recipient;
	CreateRecipient(scriptPubKey, recipient);
	vecSend.push_back(recipient);
	CRecipient aliasRecipient;
	CreateRecipient(scriptPubKeyAlias, aliasRecipient);
	vecSend.push_back(aliasRecipient);


	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);
	const CWalletTx * wtxInCert=NULL;
	const CWalletTx * wtxInOffer=NULL;
	const CWalletTx * wtxInEscrow=NULL;
	SendMoneySyscoin(vecSend, recipient.nAmount+fee.nAmount, false, wtx, wtxInOffer, wtxInCert, wtxAliasIn, wtxInEscrow);

	UniValue res(UniValue::VARR);
	res.push_back(wtx.GetHash().GetHex());
	res.push_back(HexStr(vchRand));
	return res;
}

UniValue offeraddwhitelist(const UniValue& params, bool fHelp) {
	if (fHelp || params.size() < 2 || params.size() > 3)
		throw runtime_error(
		"offeraddwhitelist <offer guid> <alias guid> [discount percentage]\n"
		"Add to the affiliate list of your offer(controls who can resell).\n"
						"<offer guid> offer guid that you are adding to\n"
						"<alias guid> alias guid representing an alias that you want to add to the affiliate list\n"
						"<discount percentage> percentage of discount given to reseller for this offer. Negative discount adds on top of offer price, acts as an extra commission. -99 to 99.\n"						
						+ HelpRequiringPassphrase());

	// gather & validate inputs
	vector<unsigned char> vchOffer = vchFromValue(params[0]);
	vector<unsigned char> vchAlias =  vchFromValue(params[1]);
	int nDiscountPctInteger = 0;
	
	if(params.size() >= 3)
		nDiscountPctInteger = atoi(params[2].get_str().c_str());

	CWalletTx wtx;

	// this is a syscoin txn
	CScript scriptPubKeyOrig;
	// create OFFERUPDATE txn key
	CScript scriptPubKey;



	EnsureWalletIsUnlocked();

	// look for a transaction with this key
	CTransaction tx;
	COffer theOffer;
	if (!GetTxOfOffer( vchOffer, theOffer, tx, true))
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 511 - Could not find an offer with this guid");

	CTransaction aliastx;
	CAliasIndex theAlias;
	const CWalletTx *wtxAliasIn = NULL;
	if (!GetTxOfAlias(theOffer.vchAlias, theAlias, aliastx, true))
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 511a - Could not find an alias with this guid");

	if(!IsSyscoinTxMine(aliastx, "alias")) {
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 511b - This alias is not yours");
	}
	wtxAliasIn = pwalletMain->GetWalletTx(aliastx.GetHash());
	if (wtxAliasIn == NULL)
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 511c - This alias is not in your wallet");
	if (ExistsInMempool(vchAlias, OP_ALIAS_ACTIVATE) || ExistsInMempool(vchAlias, OP_ALIAS_UPDATE)) {
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 511d - There are pending operations on that alias");
	}
	CPubKey currentKey(theAlias.vchPubKey);
	scriptPubKeyOrig = GetScriptForDestination(currentKey.GetID());
	CScript scriptPubKeyAlias;
	scriptPubKeyAlias << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << theOffer.vchAlias << theOffer.vchGUID << vchFromString("") << OP_2DROP << OP_2DROP;
	scriptPubKeyAlias += scriptPubKeyOrig;

	const CWalletTx* wtxIn = pwalletMain->GetWalletTx(tx.GetHash());
	if (wtxIn == NULL)
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 512 - This offer is not in your wallet");
	// check for existing pending offers
	if (ExistsInMempool(vchOffer, OP_OFFER_ACTIVATE) || ExistsInMempool(vchOffer, OP_OFFER_UPDATE)) {
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 513 - There are pending operations on that offer");
	}
	COfferLinkWhitelistEntry foundEntry;
	if(theOffer.linkWhitelist.GetLinkEntryByHash(vchAlias, foundEntry))
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 514 - This alias entry already exists on affiliate list");

	COfferLinkWhitelistEntry entry;
	entry.aliasLinkVchRand = vchAlias;
	entry.nDiscountPct = nDiscountPctInteger;
	theOffer.ClearOffer();
	theOffer.linkWhitelist.PutWhitelistEntry(entry);
	theOffer.nHeight = chainActive.Tip()->nHeight;

	
	const vector<unsigned char> &data = theOffer.Serialize();
    uint256 hash = Hash(data.begin(), data.end());
 	vector<unsigned char> vchHash = CScriptNum(hash.GetCheapHash()).getvch();
    vector<unsigned char> vchHashOffer = vchFromValue(HexStr(vchHash));
	scriptPubKey << CScript::EncodeOP_N(OP_OFFER_UPDATE) << vchOffer << vchHashOffer << OP_2DROP << OP_DROP;
	scriptPubKey += scriptPubKeyOrig;

	vector<CRecipient> vecSend;
	CRecipient recipient;
	CreateRecipient(scriptPubKey, recipient);
	vecSend.push_back(recipient);
	CRecipient aliasRecipient;
	CreateRecipient(scriptPubKeyAlias, aliasRecipient);
	vecSend.push_back(aliasRecipient);
	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);
	const CWalletTx * wtxInCert=NULL;
	const CWalletTx * wtxInEscrow=NULL;
	SendMoneySyscoin(vecSend, recipient.nAmount+fee.nAmount, false, wtx, wtxIn, wtxInCert, wtxAliasIn, wtxInEscrow);

	UniValue res(UniValue::VARR);
	res.push_back(wtx.GetHash().GetHex());
	return res;
}
UniValue offerremovewhitelist(const UniValue& params, bool fHelp) {
	if (fHelp || params.size() != 2)
		throw runtime_error(
		"offerremovewhitelist <offer guid> <alias guid>\n"
		"Remove from the affiliate list of your offer(controls who can resell).\n"
						+ HelpRequiringPassphrase());
	// gather & validate inputs
	vector<unsigned char> vchOffer = vchFromValue(params[0]);
	vector<unsigned char> vchAlias = vchFromValue(params[1]);

	CTransaction txCert;
	CCert theCert;
	CWalletTx wtx;
	const CWalletTx* wtxIn = NULL;

	// this is a syscoin txn
	CScript scriptPubKeyOrig;
	// create OFFERUPDATE txn keys
	CScript scriptPubKey;

	EnsureWalletIsUnlocked();

	// look for a transaction with this key
	CTransaction tx;
	COffer theOffer;
	if (!GetTxOfOffer( vchOffer, theOffer, tx, true))
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 515 - Could not find an offer with this guid");
	CTransaction aliastx;
	CAliasIndex theAlias;
	const CWalletTx *wtxAliasIn = NULL;
	if (!GetTxOfAlias(theOffer.vchAlias, theAlias, aliastx, true))
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 515a - Could not find an alias with this guid");

	if(!IsSyscoinTxMine(aliastx, "alias")) {
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 515b - This alias is not yours");
	}
	wtxAliasIn = pwalletMain->GetWalletTx(aliastx.GetHash());
	if (wtxAliasIn == NULL)
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 515c - This alias is not in your wallet");
	if (ExistsInMempool(vchAlias, OP_ALIAS_ACTIVATE) || ExistsInMempool(vchAlias, OP_ALIAS_UPDATE)) {
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 515d - There are pending operations on that alias");
	}
	CPubKey currentKey(theAlias.vchPubKey);
	scriptPubKeyOrig = GetScriptForDestination(currentKey.GetID());
	CScript scriptPubKeyAlias;
	scriptPubKeyAlias << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << theOffer.vchAlias << theAlias.vchGUID << vchFromString("") << OP_2DROP << OP_2DROP;
	scriptPubKeyAlias += scriptPubKeyOrig;
	wtxIn = pwalletMain->GetWalletTx(tx.GetHash());
	if (wtxIn == NULL)
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 516 - This offer is not in your wallet");
	// check for existing pending offers
	if (ExistsInMempool(vchOffer, OP_OFFER_ACTIVATE) || ExistsInMempool(vchOffer, OP_OFFER_UPDATE)) {
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 517 - There are pending operations on that offer");
	}
	// create OFFERUPDATE txn keys
	COfferLinkWhitelistEntry foundEntry;
	if(!theOffer.linkWhitelist.GetLinkEntryByHash(vchAlias, foundEntry))
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 518 - This alias entry was not found on affiliate list");
	theOffer.ClearOffer();
	theOffer.nHeight = chainActive.Tip()->nHeight;
	theOffer.linkWhitelist.PutWhitelistEntry(foundEntry);

	const vector<unsigned char> &data = theOffer.Serialize();
    uint256 hash = Hash(data.begin(), data.end());
 	vector<unsigned char> vchHash = CScriptNum(hash.GetCheapHash()).getvch();
    vector<unsigned char> vchHashOffer = vchFromValue(HexStr(vchHash));
	scriptPubKey << CScript::EncodeOP_N(OP_OFFER_UPDATE) << vchOffer << vchHashOffer << OP_2DROP << OP_DROP;
	scriptPubKey += scriptPubKeyOrig;

	vector<CRecipient> vecSend;
	CRecipient recipient;
	CreateRecipient(scriptPubKey, recipient);
	vecSend.push_back(recipient);
	CRecipient aliasRecipient;
	CreateRecipient(scriptPubKeyAlias, aliasRecipient);
	vecSend.push_back(aliasRecipient);
	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);
	const CWalletTx * wtxInEscrow=NULL;
	const CWalletTx * wtxInCert=NULL;
	SendMoneySyscoin(vecSend, recipient.nAmount+fee.nAmount, false, wtx, wtxIn, wtxInCert, wtxAliasIn, wtxInEscrow);

	UniValue res(UniValue::VARR);
	res.push_back(wtx.GetHash().GetHex());
	return res;
}
UniValue offerclearwhitelist(const UniValue& params, bool fHelp) {
	if (fHelp || params.size() != 1)
		throw runtime_error(
		"offerclearwhitelist <offer guid>\n"
		"Clear the affiliate list of your offer(controls who can resell).\n"
						+ HelpRequiringPassphrase());
	// gather & validate inputs
	vector<unsigned char> vchOffer = vchFromValue(params[0]);

	// this is a syscoind txn
	CWalletTx wtx;
	const CWalletTx* wtxIn;
	CScript scriptPubKeyOrig;

	EnsureWalletIsUnlocked();

	// look for a transaction with this key
	CTransaction tx;
	COffer theOffer;
	if (!GetTxOfOffer( vchOffer, theOffer, tx, true))
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 519 - Could not find an offer with this guid");
	CTransaction aliastx;
	CAliasIndex theAlias;
	const CWalletTx *wtxAliasIn = NULL;
	if (!GetTxOfAlias(theOffer.vchAlias, theAlias, aliastx, true))
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 519a - Could not find an alias with this guid");

	if(!IsSyscoinTxMine(aliastx, "alias")) {
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 519b - This alias is not yours");
	}
	wtxAliasIn = pwalletMain->GetWalletTx(aliastx.GetHash());
	if (wtxAliasIn == NULL)
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 519c - This alias is not in your wallet");
	if (ExistsInMempool(vchAlias, OP_ALIAS_ACTIVATE) || ExistsInMempool(vchAlias, OP_ALIAS_UPDATE)) {
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 519d - There are pending operations on that alias");
	}


	CPubKey currentKey(theAlias.vchPubKey);
	scriptPubKeyOrig = GetScriptForDestination(currentKey.GetID());

	CScript scriptPubKeyAlias;
	scriptPubKeyAlias << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << theOffer.vchAlias << theAlias.vchGUID << vchFromString("") << OP_2DROP << OP_2DROP;
	scriptPubKeyAlias += scriptPubKeyOrig;

	wtxIn = pwalletMain->GetWalletTx(tx.GetHash());
	if (wtxIn == NULL)
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 520 - This offer is not in your wallet");
	// check for existing pending offers
	if (ExistsInMempool(vchOffer, OP_OFFER_ACTIVATE) || ExistsInMempool(vchOffer, OP_OFFER_UPDATE)) {
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 521 - There are pending operations on that offer");
	}
	theOffer.ClearOffer();
	theOffer.nHeight = chainActive.Tip()->nHeight;
	// create OFFERUPDATE txn keys
	CScript scriptPubKey;

	COfferLinkWhitelistEntry entry;
	// special case to clear all entries for this offer
	entry.nDiscountPct = 127;
	theOffer.linkWhitelist.PutWhitelistEntry(entry);

	const vector<unsigned char> &data = theOffer.Serialize();
    uint256 hash = Hash(data.begin(), data.end());
 	vector<unsigned char> vchHash = CScriptNum(hash.GetCheapHash()).getvch();
    vector<unsigned char> vchHashOffer = vchFromValue(HexStr(vchHash));
	scriptPubKey << CScript::EncodeOP_N(OP_OFFER_UPDATE) << vchOffer << vchHashOffer << OP_2DROP << OP_DROP;
	scriptPubKey += scriptPubKeyOrig;

	vector<CRecipient> vecSend;
	CRecipient recipient;
	CreateRecipient(scriptPubKey, recipient);
	vecSend.push_back(recipient);

	CRecipient aliasRecipient;
	CreateRecipient(scriptPubKeyAlias, aliasRecipient);
	vecSend.push_back(aliasRecipient);

	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);
	const CWalletTx * wtxInCert=NULL;
	const CWalletTx * wtxInEscrow=NULL;
	SendMoneySyscoin(vecSend, recipient.nAmount+fee.nAmount, false, wtx, wtxIn, wtxInCert, wtxAliasIn, wtxInEscrow);

	UniValue res(UniValue::VARR);
	res.push_back(wtx.GetHash().GetHex());
	return res;
}

UniValue offerwhitelist(const UniValue& params, bool fHelp) {
    if (fHelp || 1 != params.size())
        throw runtime_error("offerwhitelist <offer guid>\n"
                "List all affiliates for this offer.\n");
    UniValue oRes(UniValue::VARR);
    vector<unsigned char> vchOffer = vchFromValue(params[0]);
	// look for a transaction with this key
	CTransaction tx;
	COffer theOffer;
	if (!GetTxOfOffer( vchOffer, theOffer, tx))
		throw runtime_error("could not find an offer with this guid");
	
	for(unsigned int i=0;i<theOffer.linkWhitelist.entries.size();i++) {
		CTransaction txAlias;
		CAliasIndex theAlias;
		COfferLinkWhitelistEntry& entry = theOffer.linkWhitelist.entries[i];
		if (GetTxOfAlias(entry.aliasLinkVchRand, theAlias, txAlias))
		{
			UniValue oList(UniValue::VOBJ);
			oList.push_back(Pair("alias", stringFromVch(entry.aliasLinkVchRand)));
			int expires_in = 0;
			uint64_t nHeight = theAlias.nHeight;
			if (!GetSyscoinTransaction(nHeight, txAlias.GetHash(), txAlias, Params().GetConsensus()))
				continue;
			
            if(nHeight + (theAlias.nRenewal*GetAliasExpirationDepth()) - chainActive.Tip()->nHeight > 0)
			{
				expires_in = nHeight + (theAlias.nRenewal*GetAliasExpirationDepth()) - chainActive.Tip()->nHeight;
			}  
			oList.push_back(Pair("expiresin",expires_in));
			oList.push_back(Pair("offer_discount_percentage", strprintf("%d%%", entry.nDiscountPct)));
			oRes.push_back(oList);
		}  
    }
    return oRes;
}

UniValue offerupdate(const UniValue& params, bool fHelp) {
	if (fHelp || params.size() < 7 || params.size() > 14)
		throw runtime_error(
		"offerupdate <aliaspeg> <alias> <guid> <category> <title> <quantity> <price> [description] [currency] [private='0'] [cert. guid=''] [exclusive resell='1'] [geolocation=''] [safesearch=Yes]\n"
						"Perform an update on an offer you control.\n"
						+ HelpRequiringPassphrase());
	// gather & validate inputs
	vector<unsigned char> vchAliasPeg = vchFromValue(params[0]);
	vector<unsigned char> vchAlias = vchFromValue(params[1]);
	vector<unsigned char> vchOffer = vchFromValue(params[2]);
	vector<unsigned char> vchCat = vchFromValue(params[3]);
	vector<unsigned char> vchTitle = vchFromValue(params[4]);
	vector<unsigned char> vchDesc;
	vector<unsigned char> vchCert;
	vector<unsigned char> vchGeoLocation;
	vector<unsigned char> sCurrencyCode;
	bool bExclusiveResell = true;
	int bPrivate = false;
	int nQty;
	double price;
	if (params.size() >= 8) vchDesc = vchFromValue(params[7]);
	if (params.size() >= 9) sCurrencyCode = vchFromValue(params[8]);
	if (params.size() >= 10) bPrivate = atoi(params[9].get_str().c_str()) == 1? true: false;
	if (params.size() >= 11) vchCert = vchFromValue(params[10]);
	if(vchCert == vchFromString("nocert"))
		vchCert.clear();
	if (params.size() >= 12) bExclusiveResell = atoi(params[11].get_str().c_str()) == 1? true: false;
	if (params.size() >= 13) vchGeoLocation = vchFromValue(params[12]);
	string strSafeSearch = "Yes";
	if(params.size() >= 14)
	{
		strSafeSearch = params[13].get_str();
	}
	try {
		nQty = atoi(params[5].get_str());
		price = atof(params[6].get_str().c_str());

	} catch (std::exception &e) {
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 522 - Invalid price and/or quantity values. Quantity must be less than 4294967296 and greater than or equal to -1");
	}

	CAliasIndex alias;
	CTransaction aliastx;
	const CWalletTx *wtxAliasIn = NULL;
	if (!GetTxOfAlias(vchAlias, alias, aliastx, true))
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 523 - Could not find an alias with this name");

	if(!IsSyscoinTxMine(aliastx, "alias")) {
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 524 - This alias is not yours");
	}
	wtxAliasIn = pwalletMain->GetWalletTx(aliastx.GetHash());
	if (wtxAliasIn == NULL)
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 524 - This alias is not in your wallet");
	if (ExistsInMempool(vchAlias, OP_ALIAS_ACTIVATE) || ExistsInMempool(vchAlias, OP_ALIAS_UPDATE)) {
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 524a - There are pending operations on that alias");
	}
	// this is a syscoind txn
	CWalletTx wtx;
	const CWalletTx* wtxIn;
	CScript scriptPubKeyOrig, scriptPubKeyCertOrig, scriptPubKeyAlias;

	EnsureWalletIsUnlocked();

	// look for a transaction with this key
	CTransaction tx, linktx;
	COffer theOffer, linkOffer;
	if (!GetTxOfOffer( vchOffer, theOffer, tx, true))
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 524 - Could not find an offer with this guid");
	
	CPubKey currentKey(alias.vchPubKey);
	scriptPubKeyOrig = GetScriptForDestination(currentKey.GetID());
	scriptPubKeyAlias << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << vchAlias << alias.vchGUID << vchFromString("") << OP_2DROP << OP_2DROP;
	scriptPubKeyAlias += scriptPubKeyOrig;
	// create OFFERUPDATE, CERTUPDATE, ALIASUPDATE txn keys
	CScript scriptPubKey, scriptPubKeyCert;

	wtxIn = pwalletMain->GetWalletTx(tx.GetHash());
	if (wtxIn == NULL)
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 524 - This offer is not in your wallet");
	// check for existing pending offers
	if (ExistsInMempool(vchOffer, OP_OFFER_ACTIVATE) || ExistsInMempool(vchOffer, OP_OFFER_UPDATE)) {
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 525 - There are pending operations on that offer");
	}

	CCert theCert;
	CTransaction txCert;
	const CWalletTx *wtxCertIn = NULL;

	// make sure this cert is still valid
	if (GetTxOfCert( vchCert, theCert, txCert, true))
	{
		// check for existing cert updates
		if (ExistsInMempool(vchCert, OP_CERT_UPDATE) || ExistsInMempool(vchCert, OP_CERT_TRANSFER)) {
			throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 526 - There are pending operations on that cert");
		}
		// make sure its in your wallet (you control this cert)		
		if (IsSyscoinTxMine(txCert, "cert")) 
		{
			CAliasIndex certAlias;
			CTransaction certaliastx;
			if (GetTxOfAlias( theCert.vchAlias, certAlias, certaliastx, true))
			{
				wtxCertIn = pwalletMain->GetWalletTx(txCert.GetHash());
				CPubKey currentCertKey(certAlias.vchPubKey);
				scriptPubKeyCertOrig = GetScriptForDestination(currentCertKey.GetID());
				scriptPubKeyCert << CScript::EncodeOP_N(OP_CERT_UPDATE) << vchCert << vchFromString("") << OP_2DROP << OP_DROP;
				scriptPubKeyCert += scriptPubKeyCertOrig;
			}
		}
		else
			throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 527 - Cannot sell this certificate, it is not yours");
	}
	
	




	COffer offerCopy = theOffer;
	theOffer.ClearOffer();
	theOffer.nHeight = chainActive.Tip()->nHeight;
	CAmount nRate;
	vector<string> rateList;
	// get precision & check for valid alias peg
	int precision = 2;
	if(vchAliasPeg.size() == 0)
		vchAliasPeg = offerCopy.vchAliasPeg;
	if(sCurrencyCode.empty() || sCurrencyCode == vchFromString("NONE"))
		sCurrencyCode = offerCopy.sCurrencyCode;
	if(getCurrencyToSYSFromAlias(vchAliasPeg, sCurrencyCode, nRate, chainActive.Tip()->nHeight, rateList,precision) != "")
	{
		string err = strprintf("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 528 - Could not find currency %s in the %s alias!\n", stringFromVch(sCurrencyCode), stringFromVch(vchAliasPeg));
		throw runtime_error(err.c_str());
	}

	double minPrice = pow(10.0,-precision);
	if(price < minPrice)
		price = minPrice;
	// update offer values
	if(offerCopy.sCategory != vchCat)
		theOffer.sCategory = vchCat;
	if(offerCopy.vchAliasPeg != vchAliasPeg)
		theOffer.vchAliasPeg = vchAliasPeg;
	if(offerCopy.sTitle != vchTitle)
		theOffer.sTitle = vchTitle;
	if(offerCopy.sDescription != vchDesc)
		theOffer.sDescription = vchDesc;
	if(offerCopy.vchGeoLocation != vchGeoLocation)
		theOffer.vchGeoLocation = vchGeoLocation;
	if(offerCopy.sCurrencyCode != sCurrencyCode)
		theOffer.sCurrencyCode = sCurrencyCode;
	if(wtxCertIn != NULL)
		theOffer.vchCert = vchCert;
	theOffer.vchAlias = alias.vchAlias;
	theOffer.safeSearch = strSafeSearch == "Yes"? true: false;
	theOffer.nQty = nQty;
	if (params.size() >= 10)
		theOffer.bPrivate = bPrivate;
	unsigned int memPoolQty = QtyOfPendingAcceptsInMempool(vchOffer);
	if(nQty != -1 && (nQty-memPoolQty) < 0)
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 529 - Not enough remaining quantity to fulfill this update");
	theOffer.nHeight = chainActive.Tip()->nHeight;
	theOffer.SetPrice(price);
	if(params.size() >= 12 && params[11].get_str().size() > 0)
		theOffer.linkWhitelist.bExclusiveResell = bExclusiveResell;



	const vector<unsigned char> &data = theOffer.Serialize();
    uint256 hash = Hash(data.begin(), data.end());
 	vector<unsigned char> vchHash = CScriptNum(hash.GetCheapHash()).getvch();
    vector<unsigned char> vchHashOffer = vchFromValue(HexStr(vchHash));
	scriptPubKey << CScript::EncodeOP_N(OP_OFFER_UPDATE) << vchOffer << vchHashOffer << OP_2DROP << OP_DROP;
	scriptPubKey += scriptPubKeyOrig;

	vector<CRecipient> vecSend;
	CRecipient recipient;
	CreateRecipient(scriptPubKey, recipient);
	vecSend.push_back(recipient);
	CRecipient certRecipient;
	CreateRecipient(scriptPubKeyCert, certRecipient);
	CRecipient aliasRecipient;
	CreateRecipient(scriptPubKeyAlias, aliasRecipient);
	vecSend.push_back(aliasRecipient);
	// if we use a cert as input to this offer tx, we need another utxo for further cert transactions on this cert, so we create one here
	if(wtxCertIn != NULL)
		vecSend.push_back(certRecipient);

	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);
	const CWalletTx * wtxInEscrow=NULL;
	SendMoneySyscoin(vecSend, recipient.nAmount+fee.nAmount, false, wtx, wtxIn, wtxCertIn, wtxAliasIn, wtxInEscrow);
	UniValue res(UniValue::VARR);
	res.push_back(wtx.GetHash().GetHex());
	return res;
}

bool CreateLinkedOfferAcceptRecipients(vector<CRecipient> &vecSend, const CAmount &nPrice, const CWalletTx* acceptTx, const vector<unsigned char>& linkedOfferGUID, const CScript& scriptPubKeyDestination, const vector<unsigned char>& vchHashOffer)
{
	unsigned int size = vecSend.size();
	vector<unsigned char> offerGUID;
	int op, nOut;
	
	vector<vector<unsigned char> > vvch;
	vector<vector<unsigned char> > vvchOffer;
	if(!linkedAcceptBlock)
		return false;
	if(!acceptTx)
		return false;
	if(!DecodeAndParseSyscoinTx(*acceptTx, op, nOut, vvch))
		return false;;
	if(op != OP_OFFER_ACCEPT)
		return false;
	offerGUID = vvch[0];
	// add recipients to vecSend if we find any linked accepts that are trying to accept linkedOfferGUID (they can be grouped into one accept for root offer owner)
	for (unsigned int i = 0; i < linkedAcceptBlock->vtx.size(); i++)
    {
		CScript scriptPubKeyAccept, scriptPubKeyPayment;
        const CTransaction &tx = linkedAcceptBlock->vtx[i];
		if(tx.nVersion != GetSyscoinTxVersion())
			continue;
		if(!DecodeAndParseSyscoinTx(tx, op, nOut, vvchOffer))
			continue;
		if(op != OP_OFFER_ACCEPT)
			continue;
		if(vvchOffer[0] != offerGUID)
			continue;
		COffer offer(tx);
		CAmount nTotalValue = ( nPrice * offer.accept.nQty );
		scriptPubKeyAccept << CScript::EncodeOP_N(OP_OFFER_ACCEPT) << linkedOfferGUID << vvchOffer[1] << vvchOffer[2] << vchHashOffer << OP_2DROP << OP_2DROP << OP_DROP; 
		scriptPubKeyAccept += scriptPubKeyDestination;
		scriptPubKeyPayment += scriptPubKeyDestination;
		CRecipient acceptRecipient;
		CreateRecipient(scriptPubKeyAccept, acceptRecipient);
		CRecipient paymentRecipient = {scriptPubKeyPayment, nTotalValue, false};
		vecSend.push_back(acceptRecipient);
		vecSend.push_back(paymentRecipient);
	}
	return vecSend.size() != size;
}

UniValue offeraccept(const UniValue& params, bool fHelp) {
	if (fHelp || 1 > params.size() || params.size() > 8)
		throw runtime_error("offeraccept <alias> <guid> [quantity] [message] [BTC TxId] [linkedacceptguidtxhash] [escrowTxHash] [justcheck]\n"
				"Accept&Pay for a confirmed offer.\n"
				"<alias> An alias of the buyer.\n"
				"<guid> guidkey from offer.\n"
				"<quantity> quantity to buy. Defaults to 1.\n"
				"<message> payment message to seller, 1KB max.\n"
				"<BTC TxId> If you have paid in Bitcoin and the offer is in Bitcoin, enter the transaction ID here. Default is empty.\n"
				"<linkedacceptguidtxhash> transaction id of the linking offer accept. For internal use only, leave blank\n"
				"<escrowTxHash> If this offer accept is done by an escrow release. For internal use only, leave blank\n"
				"<justcheck> Do not send transaction. For validation only. For internal use only, leave blank\n"
				+ HelpRequiringPassphrase());
	CSyscoinAddress refundAddr;	
	vector<unsigned char> vchAlias = vchFromValue(params[0]);
	vector<unsigned char> vchOffer = vchFromValue(params[1]);
	vector<unsigned char> vchBuyerAlias = vchAlias;
	vector<unsigned char> vchBTCTxId = vchFromValue(params.size()>=5?params[4]:"");
	vector<unsigned char> vchLinkOfferAcceptTxHash = vchFromValue(params.size()>= 6? params[5]:"");
	vector<unsigned char> vchMessage = vchFromValue(params.size()>=4?params[3]:"");
	vector<unsigned char> vchEscrowTxHash = vchFromValue(params.size()>=7?params[6]:"");
	string justCheck = params.size()>=8?params[7].get_str():"0";
	int64_t nHeight = chainActive.Tip()->nHeight;
	unsigned int nQty = 1;
	if (params.size() >= 3) {
		if(atof(params[2].get_str().c_str()) <= 0)
			throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 530 - Invalid quantity value, must be greator than 0");
	
		try {
			nQty = boost::lexical_cast<unsigned int>(params[2].get_str());
		} catch (std::exception &e) {
			throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 531 - Quantity must be less than 4294967296");
		}
	}

	// this is a syscoin txn
	CWalletTx wtx;
	CScript scriptPubKeyOrig, scriptPubKeyAliasOrig;
	// generate offer accept identifier and hash
	int64_t rand = GetRand(std::numeric_limits<int64_t>::max());
	vector<unsigned char> vchAcceptRand = CScriptNum(rand).getvch();
	vector<unsigned char> vchAccept = vchFromString(HexStr(vchAcceptRand));

	// create OFFERACCEPT txn keys
	CScript scriptPubKeyAccept, scriptPubKeyPayment;
	CScript scriptPubKeyEscrowBuyer, scriptPubKeyEscrowSeller, scriptPubKeyEscrowArbiter , scriptPubKeyAlias;
	EnsureWalletIsUnlocked();
	CTransaction acceptTx;
	COffer theOffer;
	const CWalletTx *wtxOfferIn = NULL;
	// if this is a linked offer accept, set the height to the first height so sys_rates price will match what it was at the time of the original accept
	CTransaction tx;
	vector<COffer> vtxPos;
	if (!GetTxAndVtxOfOffer( vchOffer, theOffer, tx, vtxPos, true))
	{
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 533 - Could not find an offer with this identifier");
	}
	// create accept
	COfferAccept txAccept;
	if (!vchLinkOfferAcceptTxHash.empty())
	{
		uint256 linkTxHash(uint256S(stringFromVch(vchLinkOfferAcceptTxHash)));
		wtxOfferIn = pwalletMain->GetWalletTx(linkTxHash);
		if (wtxOfferIn == NULL)
			throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 534 - This offer accept is not in your wallet");
		COffer linkOffer(*wtxOfferIn);
		if(linkOffer.accept.IsNull())
			throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 535 - Offer accept passed into the function is not actually an offer accept");
		int op, nOut;
		vector<vector<unsigned char> > vvch;
		if (!DecodeOfferTx(*wtxOfferIn, op, nOut, vvch) 
    		|| op != OP_OFFER_ACCEPT)
			throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 536 - Could not decode linked offer accept tx");
				
		// ensure both accepts have the escrow information
		txAccept.vchEscrow = linkOffer.accept.vchEscrow;
		txAccept.vchLinkAccept = linkOffer.accept.vchAcceptRand;
		txAccept.vchLinkOffer = vvch[0];
		nHeight = linkOffer.accept.nAcceptHeight;
		nQty = linkOffer.accept.nQty;
		vchAccept = linkOffer.accept.vchAcceptRand;
		vchBuyerAlias = linkOffer.accept.vchBuyerAlias;
	}
	const CWalletTx *wtxEscrowIn = NULL;
	CEscrow escrow;
	vector<vector<unsigned char> > escrowVvch;
	vector<unsigned char> vchEscrowWhitelistAlias;
	if(!vchEscrowTxHash.empty())
	{
		uint256 escrowTxHash(uint256S(stringFromVch(vchEscrowTxHash)));
		// make sure escrow is in wallet
		wtxEscrowIn = pwalletMain->GetWalletTx(escrowTxHash);
		if (wtxEscrowIn == NULL) 
			throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 537 - Release escrow transaction is not in your wallet");
		if(!escrow.UnserializeFromTx(*wtxEscrowIn))
			throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 538 - Release escrow transaction cannot unserialize escrow value");
		
		int op, nOut;
		if (!DecodeEscrowTx(*wtxEscrowIn, op, nOut, escrowVvch))
			throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 539 - Cannot decode escrow tx hash");
		
		// if we want to accept an escrow release or we are accepting a linked offer from an escrow release. Override heightToCheckAgainst if using escrow since escrow can take a long time.
		// get escrow activation
		vector<CEscrow> escrowVtxPos;
		CTransaction escrowTx;
		if (GetTxAndVtxOfEscrow( escrowVvch[0], escrow, escrowTx, escrowVtxPos))
		{
			CScript scriptPubKeyEscrowBuyerDestination, scriptPubKeyEscrowSellerDestination, scriptPubKeyEscrowArbiterDestination;
			// we want the initial funding escrow transaction height as when to calculate this offer accept price from convertCurrencyCodeToSyscoin()
			CEscrow fundingEscrow = escrowVtxPos.front();
			// update height if it is bigger than escrow creation height, we want earlier of two, linked height or escrow creation to index into sysrates check
			if(nHeight > fundingEscrow.nHeight)
				nHeight = fundingEscrow.nHeight;
			CAliasIndex arbiterAlias, buyerAlias, sellerAlias;
			CTransaction aliastx;
			GetTxOfAlias(fundingEscrow.vchArbiterAlias, arbiterAlias, aliastx, true);
			CPubKey arbiterKey(arbiterAlias.vchPubKey);

			GetTxOfAlias(fundingEscrow.vchBuyerAlias, buyerAlias, aliastx, true);
			CPubKey buyerKey(buyerAlias.vchPubKey);

			GetTxOfAlias(fundingEscrow.vchSellerAlias, sellerAlias, aliastx, true);
			CPubKey sellerKey(sellerAlias.vchPubKey);

			scriptPubKeyEscrowSellerDestination = GetScriptForDestination(sellerKey.GetID());
			scriptPubKeyEscrowBuyerDestination = GetScriptForDestination(buyerKey.GetID());
			scriptPubKeyEscrowArbiterDestination = GetScriptForDestination(arbiterKey.GetID());
			scriptPubKeyEscrowBuyer << CScript::EncodeOP_N(OP_ESCROW_COMPLETE) << escrowVvch[0] << vchFromString("0") << vchFromString("") << OP_2DROP << OP_2DROP;
			scriptPubKeyEscrowBuyer += scriptPubKeyEscrowBuyerDestination;
			scriptPubKeyEscrowSeller << CScript::EncodeOP_N(OP_ESCROW_COMPLETE) << escrowVvch[0] << vchFromString("0") << vchFromString("") << OP_2DROP << OP_2DROP;
			scriptPubKeyEscrowSeller += scriptPubKeyEscrowSellerDestination;
			scriptPubKeyEscrowArbiter << CScript::EncodeOP_N(OP_ESCROW_COMPLETE) << escrowVvch[0] << vchFromString("0") << vchFromString("") << OP_2DROP << OP_2DROP;
			scriptPubKeyEscrowArbiter += scriptPubKeyEscrowArbiterDestination;
			txAccept.vchEscrow = escrowVvch[0]; 		
		}	
	}

	if (ExistsInMempool(vchOffer, OP_OFFER_ACTIVATE)) {
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 540 - There are pending operations on that offer");
	}

	// get offer price at the time of accept from buyer
	theOffer.nHeight = nHeight;
	theOffer.GetOfferFromList(vtxPos);

	CTransaction aliastx,buyeraliastx;
	CAliasIndex theAlias,tmpAlias;
	bool isExpired = false;
	vector<CAliasIndex> aliasVtxPos;
	if(GetTxAndVtxOfAlias(theOffer.vchAlias, theAlias, aliastx, aliasVtxPos, isExpired, true))
	{
		// find the alias (for the right pubkey) at the time of linked accept/escrow if applicable
		// need this because alias can be transferred and the payment message ends up going to new alias pubkey if we dont do this. 
		// should be sent to same person that owned the offer when payment was made by buyer
		theAlias.nHeight = nHeight;
		theAlias.GetAliasFromList(aliasVtxPos);
	}
	else
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 566 - Could not find the alias associated with this offer");
	CAliasIndex buyerAlias;
	if (!GetTxOfAlias(vchBuyerAlias, buyerAlias, aliastx, true))
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 532 - Could not find buyer alias with this name");
	// get buyer passed into rpc (not necessarily actual buyer which for linked offers is the buyer of the linked offer accept)
	CAliasIndex buyerAlias1;
	if (!GetTxOfAlias(vchAlias, buyerAlias1, buyeraliastx, true))
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 532a - Could not find buyer alias with this name");
	// if not escrow accept then make sure you can't buy your own offer
	if(vchEscrowTxHash.empty())
	{
		CPubKey sellerKey = CPubKey(theAlias.vchPubKey);
		CSyscoinAddress sellerAddress(sellerKey.GetID());
		if(!sellerAddress.IsValid())
			throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 541 - Seller address is invalid");
		CKeyID keyID;
		if (!sellerAddress.GetKeyID(keyID))
			throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 542 - Seller address does not refer to a key");
		CKey vchSecret;
		if (pwalletMain->GetKey(keyID, vchSecret))
			throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 543 - Cannot purchase your own offer");
	}


	const CWalletTx *wtxAliasIn = NULL;

	COfferLinkWhitelistEntry foundEntry;
	theOffer.linkWhitelist.GetLinkEntryByHash(vchAlias, foundEntry);
	// only non linked offers can have discounts applied via whitelist for buyer
	if(theOffer.vchLinkOffer.empty() && !foundEntry.IsNull())
	{
		// check for existing alias updates/transfers
		if (ExistsInMempool(vchAlias, OP_ALIAS_UPDATE)) {
			throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 544 - There is are pending operations on that alias");
		}
		// make sure its in your wallet (you control this alias)
		if (IsSyscoinTxMine(buyeraliastx, "alias")) 
		{
			wtxAliasIn = pwalletMain->GetWalletTx(buyeraliastx.GetHash());		
			CPubKey currentKey(buyerAlias1.vchPubKey);
			scriptPubKeyAliasOrig = GetScriptForDestination(currentKey.GetID());
			scriptPubKeyAlias << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << vchAlias  << buyerAlias1.vchGUID << vchFromString("") << OP_2DROP << OP_2DROP;
			scriptPubKeyAlias += scriptPubKeyAliasOrig;
		}		
		
	}

	unsigned int memPoolQty = QtyOfPendingAcceptsInMempool(vchOffer);
	if(vtxPos.back().nQty != -1 && vtxPos.back().nQty < ((!vchEscrowTxHash.empty()? 0: nQty)+memPoolQty))
		throw runtime_error(strprintf("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 545 - Not enough remaining quantity to fulfill this orderaccept, qty remaining %u, qty desired %u,  qty waiting to be accepted by the network %d", vtxPos.back().nQty, nQty, memPoolQty));

	int precision = 2;
	CAmount nPrice = convertCurrencyCodeToSyscoin(theOffer.vchAliasPeg, theOffer.sCurrencyCode, theOffer.GetPrice(foundEntry), nHeight, precision);
	if(nPrice == 0)
		throw runtime_error(strprintf("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 546 - %s currency not found in offer's alias peg %s", stringFromVch(theOffer.sCurrencyCode), stringFromVch(theOffer.vchAliasPeg)));
	string strCipherText = "";
	// encryption should only happen once even when not a resell or not an escrow accept. It is already encrypted in both cases.
	if(vchLinkOfferAcceptTxHash.empty() && vchEscrowTxHash.empty())
	{
		if(!theOffer.vchLinkOffer.empty())
		{
			CTransaction linktx;
			COffer linkedOffer;
			if (!GetTxOfOffer( theOffer.vchLinkOffer, linkedOffer, linktx, true))
			{
				throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 547 - Could not find linked offer with this identifier");
			}
			CAliasIndex theLinkedAlias;
			if (!GetTxOfAlias( vchAlias, theLinkedAlias, aliastx, true))
				throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 567 - Could not find an alias with this guid");

			// encrypt to root offer owner if this is a linked offer you are accepting
			if(!EncryptMessage(theLinkedAlias.vchPubKey, vchMessage, strCipherText))
				throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 548 - Could not encrypt message to seller");
				
		}
		else
		{
			// encrypt to offer owner
			if(!EncryptMessage(theAlias.vchPubKey, vchMessage, strCipherText))
				throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 549 - Could not encrypt message to seller");
		}
	}
	vector<unsigned char> vchPaymentMessage;
	if(strCipherText.size() > 0)
		vchPaymentMessage = vchFromString(strCipherText);
	else
		vchPaymentMessage = vchMessage;

	if(!theOffer.vchCert.empty())
	{
		// check for existing cert transfer
		if (ExistsInMempool(theOffer.vchCert, OP_CERT_TRANSFER)) {
			throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 550 - There is a pending transfer operation on that cert");
		}	
	}
	txAccept.vchAcceptRand = vchAccept;
	txAccept.nQty = nQty;
	txAccept.nPrice = theOffer.GetPrice(foundEntry);
	// if we have a linked offer accept then use height from linked accept (the one buyer makes, not the reseller). We need to do this to make sure we convert price at the time of initial buyer's accept.
	// in checkescrowinput we override this if its from an escrow release, just like above.
	txAccept.nAcceptHeight = nHeight;
	txAccept.vchBuyerAlias = vchBuyerAlias;
	txAccept.vchMessage = vchPaymentMessage;
    CAmount nTotalValue = ( nPrice * nQty );
    
	// send one to ourselves to we can leave feedback (notice the last opcode is 1 to denote its a special feedback output for the buyer to be able to leave feedback first and not a normal accept output)
	CScript scriptPubKeyBuyer, scriptPubKeyBuyerDestination;
	CPubKey buyerKey(buyerAlias.vchPubKey);
	scriptPubKeyBuyerDestination= GetScriptForDestination(buyerKey.GetID());
	CRecipient recipientBuyer;
	if(!vchBTCTxId.empty())
	{
		uint256 txBTCId(uint256S(stringFromVch(vchBTCTxId)));
		txAccept.txBTCId = txBTCId;
	}
	COffer copyOffer = theOffer;
	theOffer.ClearOffer();
	theOffer.accept = txAccept;
	theOffer.nHeight = chainActive.Tip()->nHeight;
	if(wtxAliasIn != NULL)
		theOffer.vchLinkAlias = vchAlias;
	const vector<unsigned char> &data = theOffer.Serialize();
    uint256 hash = Hash(data.begin(), data.end());
 	vector<unsigned char> vchHash = CScriptNum(hash.GetCheapHash()).getvch();
    vector<unsigned char> vchHashOffer = vchFromValue(HexStr(vchHash));
	// if this is a linked accept then we create output for feedback for the reseller since the normal accept from buyer is used here by the linked accept
	scriptPubKeyBuyer  << CScript::EncodeOP_N(OP_OFFER_ACCEPT) << vchOffer << vchAccept << vchFromString("1") << vchHashOffer << OP_2DROP << OP_2DROP << OP_DROP;
	// normal accept sig
	scriptPubKeyAccept << CScript::EncodeOP_N(OP_OFFER_ACCEPT) << vchOffer << vchAccept << vchFromString("0") << vchHashOffer << OP_2DROP << OP_2DROP << OP_DROP;
	

	scriptPubKeyBuyer += scriptPubKeyBuyerDestination;
	CreateRecipient(scriptPubKeyBuyer, recipientBuyer);


    CScript scriptPayment;
	CPubKey currentKey(theAlias.vchPubKey);
	scriptPayment = GetScriptForDestination(currentKey.GetID());
	scriptPubKeyAccept += scriptPayment;
	scriptPubKeyPayment += scriptPayment;



	vector<CRecipient> vecSend;


	CRecipient acceptRecipient;
	CreateRecipient(scriptPubKeyAccept, acceptRecipient);
	CRecipient paymentRecipient = {scriptPubKeyPayment, nTotalValue, false};
	CRecipient aliasRecipient;
	CreateRecipient(scriptPubKeyAlias, aliasRecipient);
	CRecipient escrowBuyerRecipient;
	CreateRecipient(scriptPubKeyEscrowBuyer, escrowBuyerRecipient);
	CRecipient escrowSellerRecipient;
	CreateRecipient(scriptPubKeyEscrowSeller, escrowSellerRecipient);
	CRecipient escrowArbiterRecipient;
	CreateRecipient(scriptPubKeyEscrowArbiter, escrowArbiterRecipient);

	// send back to yourself always for feedback unless its escrow or its a non linked offer (you get the feedback output when the linked accept occurs)
	if(vchEscrowTxHash.empty() && copyOffer.vchLinkOffer.empty())
		vecSend.push_back(recipientBuyer);
	// if we are accepting an escrow transaction then create another escrow utxo for escrowcomplete to be able to do its thing
	if (wtxEscrowIn != NULL) 
	{
		vecSend.push_back(escrowBuyerRecipient);
		vecSend.push_back(escrowSellerRecipient);
		vecSend.push_back(escrowArbiterRecipient);
	}
	// if we use a alias as input to this offer tx, we need another utxo for further alias transactions on this alias, so we create one here
	if(wtxAliasIn != NULL)
	{
		vecSend.push_back(aliasRecipient);
	}

	// check for Bitcoin payment on the bitcoin network, otherwise pay in syscoin
	if(!vchBTCTxId.empty() && stringFromVch(copyOffer.sCurrencyCode) == "BTC")
	{

	}
	else if(!copyOffer.bOnlyAcceptBTC)
	{
		// linked accept will go through the linkedAcceptBlock and find all linked accepts to same offer and group them together into vecSend so it can go into one tx (inputs can be shared, mainly the whitelist alias inputs)
		if(!CreateLinkedOfferAcceptRecipients(vecSend, nPrice, wtxOfferIn, vchOffer, scriptPayment, vchHashOffer))
		{
			vecSend.push_back(paymentRecipient);
			vecSend.push_back(acceptRecipient);
		}
	}
	else
	{
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 551 - This offer must be paid with Bitcoins as per requirements of the seller");
	}

	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);
	const CWalletTx * wtxInCert=NULL;

	// if making a purchase and we are using an alias from the whitelist of the offer, we may need to prove that we own that alias so in that case we attach an input from the alias
	// if purchasing an escrow, we adjust the height to figure out pricing of the accept so we may also attach escrow inputs to the tx
	SendMoneySyscoin(vecSend, recipientBuyer.nAmount+acceptRecipient.nAmount+paymentRecipient.nAmount+fee.nAmount+escrowBuyerRecipient.nAmount+escrowArbiterRecipient.nAmount+escrowSellerRecipient.nAmount+aliasRecipient.nAmount, false, wtx, wtxOfferIn, wtxInCert, wtxAliasIn, wtxEscrowIn, true, justCheck);
	
	UniValue res(UniValue::VARR);
	res.push_back(wtx.GetHash().GetHex());
	res.push_back(stringFromVch(vchAccept));
	return res;
}

void HandleAcceptFeedback(const COfferAccept& accept, const COffer& offer)
{
	if(accept.feedback.nRating > 0)
	{
		string aliasStr;
		CPubKey key;
		if(accept.feedback.nFeedbackUser == ACCEPTBUYER)
			aliasStr = stringFromVch(accept.vchBuyerAlias);
		else if(accept.feedback.nFeedbackUser == ACCEPTSELLER)
			aliasStr = stringFromVch(offer.vchAlias);
		CSyscoinAddress address = CSyscoinAddress(aliasStr);
		if(address.IsValid() && address.isAlias)
		{
			vector<CAliasIndex> vtxPos;
			const vector<unsigned char> &vchAlias = vchFromString(address.aliasName);
			if (paliasdb->ReadAlias(vchAlias, vtxPos) && !vtxPos.empty())
			{
				
				CAliasIndex alias = vtxPos.back();
				alias.nRatingCount++;
				alias.nRating += accept.feedback.nRating;
				PutToAliasList(vtxPos, alias);
				paliasdb->WriteAlias(vchAlias, vchFromString(address.ToString()), vtxPos);
			}
		}
			
	}
}
int FindFeedbackInAccept(const vector<unsigned char> &vchAccept, const unsigned char nFeedbackUser, const vector<COffer> &vtxPos, int& numRatings)
{
	int count = 0;
	numRatings = 0;
	for(unsigned int i =0;i<vtxPos.size();i++)
	{	
		if(!vtxPos[i].accept.feedback.IsNull() && vtxPos[i].accept.vchAcceptRand == vchAccept && vtxPos[i].accept.feedback.nFeedbackUser == nFeedbackUser)
		{
			count++;
			if(vtxPos[i].accept.feedback.nRating > 0)
				numRatings++;
		}
	}
	return count;
}
void GetFeedbackInAccept(vector<CAcceptFeedback> &feedBack, int &avgRating, const vector<unsigned char> &vchAccept, const AcceptUser type, const vector<COffer> &vtxPos)
{
	float nRating = 0;
	int nRatingCount = 0;
	for(unsigned int i =0;i<vtxPos.size();i++)
	{
		if(!vtxPos[i].accept.feedback.IsNull() && vtxPos[i].accept.vchAcceptRand == vchAccept && vtxPos[i].accept.feedback.nFeedbackUser == type)
		{
			if(vtxPos[i].accept.feedback.nRating > 0)
			{
				nRating += vtxPos[i].accept.feedback.nRating;
				nRatingCount++;
			}
			feedBack.push_back(vtxPos[i].accept.feedback);
		}
	}
	if(nRatingCount > 0)
	{
		nRating /= nRatingCount;
	}
	avgRating = (int)roundf(nRating);
	if(feedBack.size() > 0)
		sort(feedBack.begin(), feedBack.end(), acceptfeedbacksort());
	
}
UniValue offeracceptfeedback(const UniValue& params, bool fHelp) {
    if (fHelp || params.size() != 4)
        throw runtime_error(
		"offeracceptfeedback <offer guid> <offeraccept guid> [feedback] [rating] \n"
                        "Send feedback and rating for offer accept specified. Ratings are numbers from 1 to 5\n"
                        + HelpRequiringPassphrase());
   // gather & validate inputs
	int nRating = 0;
	vector<unsigned char> vchOffer = vchFromValue(params[0]);
	vector<unsigned char> vchAcceptRand = vchFromValue(params[1]);
	vector<unsigned char> vchFeedback = vchFromValue(params[2]);
	try {
		nRating = atoi(params[3].get_str());
		if(nRating < 0 || nRating > 5)
			throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 552 - Invalid rating value, must be less than or equal to 5 and greater than or equal to 0");

	} catch (std::exception &e) {
		throw runtime_error("invalid rating value");
	}
	
	
    // this is a syscoin transaction
    CWalletTx wtx;

	EnsureWalletIsUnlocked();

    // look for a transaction with this key
    CTransaction tx;
	COffer offer;
	COfferAccept theOfferAccept;
	const CWalletTx *wtxIn;
	// feedback is skipped by default but we want to use it as input to another feedback possibly if this is a reply
	bool skipFeedback = false;
	if (!GetTxOfOfferAccept(vchOffer, vchAcceptRand, offer, theOfferAccept, tx, skipFeedback))
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 553 - Could not find this offer accept");

	wtxIn = pwalletMain->GetWalletTx(tx.GetHash());
	if (wtxIn == NULL)
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 554 - This offer accept is not in your wallet");
	vector<vector<unsigned char> > vvch;
    int op, nOut;
    if (!DecodeOfferTx(tx, op, nOut, vvch) 
    	|| op != OP_OFFER_ACCEPT)
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 555 - Could not decode offer accept tx");
	
	CAliasIndex buyerAlias, sellerAlias;
	CTransaction aliastx;

	GetTxOfAlias(theOfferAccept.vchBuyerAlias, buyerAlias, aliastx, true);
	CPubKey buyerKey(buyerAlias.vchPubKey);
	CSyscoinAddress buyerAddress(buyerKey.GetID());

	GetTxOfAlias(offer.vchAlias, sellerAlias, aliastx, true);
	CPubKey sellerKey(sellerAlias.vchPubKey);
	CSyscoinAddress sellerAddress(sellerKey.GetID());


	bool foundBuyerKey = false;
	try
	{
		CKeyID keyID;
		if (!buyerAddress.GetKeyID(keyID))
			throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 556 - Buyer address does not refer to a key");
		CKey vchSecret;
		if (!pwalletMain->GetKey(keyID, vchSecret))
			throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 557 - Private key for buyer address is not know");
		foundBuyerKey = true;
	}
	catch(...)
	{
		foundBuyerKey = false;
	}
	bool foundSellerKey = false;
	try
	{
		CKeyID keyID;
		if (!sellerAddress.GetKeyID(keyID))
			throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 558 - Seller address does not refer to a key");
		CKey vchSecret;
		if (!pwalletMain->GetKey(keyID, vchSecret))
			throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 559 - Private key for seller address is not known");
		foundSellerKey = true;
	}
	catch(...)
	{
		foundSellerKey = false;
	}
	
     	// check for existing escrow 's
	if (ExistsInMempool(vvch[0], OP_OFFER_ACCEPT)) {
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 560 - There are pending operations on that offer");
	}
	offer.ClearOffer();
	offer.accept = theOfferAccept;
	offer.nHeight = chainActive.Tip()->nHeight;
	// buyer
	if(foundBuyerKey)
	{
		CAcceptFeedback sellerFeedback(ACCEPTSELLER);
		sellerFeedback.vchFeedback = vchFeedback;
		sellerFeedback.nRating = nRating;
		sellerFeedback.nHeight = chainActive.Tip()->nHeight;
		offer.accept.feedback = sellerFeedback;
	}
	// seller
	else if(foundSellerKey)
	{
		CAcceptFeedback buyerFeedback(ACCEPTBUYER);
		buyerFeedback.vchFeedback = vchFeedback;
		buyerFeedback.nRating = nRating;
		buyerFeedback.nHeight = chainActive.Tip()->nHeight;
		offer.accept.feedback = buyerFeedback;	
	}
	else
	{
		throw runtime_error("SYSCOIN_OFFER_RPC_ERROR ERRCODE: 561 - You must be either the buyer or seller to leave feedback on this offer purchase");
	}

	const vector<unsigned char> &data = offer.Serialize();
    uint256 hash = Hash(data.begin(), data.end());
 	vector<unsigned char> vchHash = CScriptNum(hash.GetCheapHash()).getvch();
    vector<unsigned char> vchHashOffer = vchFromValue(HexStr(vchHash));

	CScript scriptPubKeyBuyer, scriptPubKeySeller,scriptPubKeyBuyerDestination, scriptPubKeySellerDestination;
	scriptPubKeyBuyerDestination= GetScriptForDestination(buyerKey.GetID());
	scriptPubKeySellerDestination= GetScriptForDestination(sellerKey.GetID());
	vector<CRecipient> vecSend;
	CRecipient recipientBuyer, recipientSeller;
	scriptPubKeyBuyer << CScript::EncodeOP_N(OP_OFFER_ACCEPT) << vvch[0] << vvch[1] << vvch[2] << vchHashOffer << OP_2DROP <<  OP_2DROP << OP_DROP;
	scriptPubKeyBuyer += scriptPubKeyBuyerDestination;
	CreateRecipient(scriptPubKeyBuyer, recipientBuyer);
	scriptPubKeySeller << CScript::EncodeOP_N(OP_OFFER_ACCEPT) << vvch[0] << vvch[1] << vvch[2] << vchHashOffer << OP_2DROP << OP_2DROP << OP_DROP;
	scriptPubKeySeller += scriptPubKeySellerDestination;
	CreateRecipient(scriptPubKeySeller, recipientSeller);
	// buyer
	if(foundBuyerKey)
	{
		vecSend.push_back(recipientSeller);

	}
	// seller
	else if(foundSellerKey)
	{
		vecSend.push_back(recipientBuyer);

	}
	
	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);

	const CWalletTx * wtxInEscrow=NULL;
	const CWalletTx * wtxInCert=NULL;
	const CWalletTx * wtxInAlias=NULL;
	SendMoneySyscoin(vecSend, recipientBuyer.nAmount+recipientSeller.nAmount+fee.nAmount, false, wtx, wtxIn, wtxInCert, wtxInAlias, wtxInEscrow);
	UniValue ret(UniValue::VARR);
	ret.push_back(wtx.GetHash().GetHex());
	return ret;
}
UniValue offerinfo(const UniValue& params, bool fHelp) {
	if (fHelp || 1 != params.size())
		throw runtime_error("offerinfo <guid>\n"
				"Show values of an offer.\n");

	UniValue oLastOffer(UniValue::VOBJ);
	vector<unsigned char> vchOffer = vchFromValue(params[0]);
	string offer = stringFromVch(vchOffer);
	COffer theOffer;
	vector<COffer> vtxPos, vtxLinkPos;
	if (!pofferdb->ReadOffer(vchOffer, vtxPos))
		throw runtime_error("failed to read from offer DB");
	if (vtxPos.size() < 1)
		throw runtime_error("no result returned");

    // get transaction pointed to by offer
	CTransaction tx;
	if(!GetTxOfOffer( vchOffer, theOffer, tx, true))
		throw runtime_error("failed to read offer transaction from disk");
	if(theOffer.safetyLevel >= SAFETY_LEVEL2)
		throw runtime_error("offer has been banned");
	CTransaction linkTx;
	COffer linkOffer;
	if( !theOffer.vchLinkOffer.empty())
	{
		if(!GetTxOfOffer( theOffer.vchLinkOffer, linkOffer, linkTx, true))
			throw runtime_error("failed to read linked offer transaction from disk");
	}


	// check that the seller isn't banned level 2
	vector<CAliasIndex> vtxAliasPos;
	if (!paliasdb->ReadAlias(theOffer.vchAlias, vtxAliasPos))
		throw runtime_error("failed to read seller alias from alias DB");
	if (vtxAliasPos.size() < 1)
		throw runtime_error("no seller found for this offer");
	if(vtxAliasPos.back().safetyLevel >= SAFETY_LEVEL2)
		throw runtime_error("offer owner has been banned");
	vector<vector<unsigned char> > vvch;
    int op, nOut;
	UniValue oOffer(UniValue::VOBJ);
	vector<unsigned char> vchValue;
	UniValue aoOfferAccepts(UniValue::VARR);
	for(int i=vtxPos.size()-1;i>=0;i--) {
		COfferAccept ca = vtxPos[i].accept;
		COffer acceptOffer;
		acceptOffer.nHeight = ca.nAcceptHeight;
		if(ca.IsNull())
			continue;
		if(!ca.feedback.IsNull())
			continue;
		if(!acceptOffer.GetOfferFromList(vtxPos))
			continue;
		UniValue oOfferAccept(UniValue::VOBJ);

        // get transaction pointed to by offer

        CTransaction txA;
        uint256 txHashA= ca.txHash;
        if (!GetSyscoinTransaction(ca.nHeight, txHashA, txA, Params().GetConsensus()))
		{
			error(strprintf("failed to accept read transaction from disk: %s", txHashA.GetHex()).c_str());
			continue;
		}
		for (unsigned int j = 0; j < txA.vout.size(); j++)
		{
			if (!IsSyscoinScript(txA.vout[j].scriptPubKey, op, vvch))
				continue;
			if(op != OP_OFFER_ACCEPT)
				continue;
			if(vvch[2] == vchFromString("1"))
				continue;
			if(ca.vchAcceptRand == vvch[1])
				break;
		}
		if(op != OP_OFFER_ACCEPT)
			continue;

		const vector<unsigned char> &vchAcceptRand = vvch[1];		
		string sTime;
		CBlockIndex *pindex = chainActive[ca.nHeight];
		if (pindex) {
			sTime = strprintf("%llu", pindex->nTime);
		}
		int avgBuyerRating, avgSellerRating;
		vector<CAcceptFeedback> buyerFeedBacks, sellerFeedBacks;
		if( !theOffer.vchLinkOffer.empty())
		{
			GetFeedbackInAccept(buyerFeedBacks, avgBuyerRating, ca.vchAcceptRand,ACCEPTBUYER, vtxLinkPos);
			GetFeedbackInAccept(sellerFeedBacks, avgSellerRating, ca.vchAcceptRand,ACCEPTSELLER, vtxLinkPos);
		}
		else
		{
			GetFeedbackInAccept(buyerFeedBacks, avgBuyerRating, ca.vchAcceptRand,ACCEPTBUYER, vtxPos);
			GetFeedbackInAccept(sellerFeedBacks, avgSellerRating, ca.vchAcceptRand,ACCEPTSELLER, vtxPos);
		}
        string sHeight = strprintf("%llu", ca.nHeight);
		oOfferAccept.push_back(Pair("id", stringFromVch(vchAcceptRand)));
		oOfferAccept.push_back(Pair("txid", ca.txHash.GetHex()));
		string strBTCId = "";
		if(!ca.txBTCId.IsNull())
			strBTCId = ca.txBTCId.GetHex();
		oOfferAccept.push_back(Pair("btctxid", strBTCId));
		oOfferAccept.push_back(Pair("height", sHeight));
		oOfferAccept.push_back(Pair("time", sTime));
		oOfferAccept.push_back(Pair("quantity", strprintf("%d", ca.nQty)));
		oOfferAccept.push_back(Pair("currency", stringFromVch(acceptOffer.sCurrencyCode)));
		vector<unsigned char> vchOfferAcceptLink;
		bool foundOffer = false;
		for (unsigned int j = 0; j < txA.vin.size(); j++) {
			vector<vector<unsigned char> > vvchIn;
			int opIn;
			const COutPoint *prevOutput = &txA.vin[j].prevout;
			if(!GetPreviousInput(prevOutput, opIn, vvchIn))
				continue;
			if(foundOffer)
				break;

			if (!foundOffer && IsOfferOp(opIn)) {
				if(opIn == OP_OFFER_ACCEPT && vvchIn[2] != vchFromString("1"))
				{
					vchOfferAcceptLink = vvchIn[1];
					foundOffer = true; 
				}
			}
		}
		
		string linkAccept = "";
		if(!vchOfferAcceptLink.empty())
			linkAccept = stringFromVch(vchOfferAcceptLink);
		oOfferAccept.push_back(Pair("linkofferaccept", linkAccept));
		if(!FindOfferAcceptPayment(txA, ca.nPrice) && ca.txBTCId.IsNull())
			continue;
		oOfferAccept.push_back(Pair("offer_discount_percentage", strprintf("%.2f%%", 100.0f - 100.0f*(ca.nPrice/theOffer.GetPrice()))));			
		oOfferAccept.push_back(Pair("escrowlink", stringFromVch(ca.vchEscrow)));
		int precision = 2;
		CAmount nPricePerUnit = convertCurrencyCodeToSyscoin(acceptOffer.vchAliasPeg, acceptOffer.sCurrencyCode, ca.nPrice, ca.nAcceptHeight-1, precision);
		oOfferAccept.push_back(Pair("systotal", ValueFromAmount(nPricePerUnit * ca.nQty)));
		oOfferAccept.push_back(Pair("sysprice", ValueFromAmount(nPricePerUnit)));
		oOfferAccept.push_back(Pair("price", strprintf("%.*f", precision, ca.nPrice ))); 	
		oOfferAccept.push_back(Pair("total", strprintf("%.*f", precision, ca.nPrice * ca.nQty )));
		oOfferAccept.push_back(Pair("buyer", stringFromVch(ca.vchBuyerAlias)));
		oOfferAccept.push_back(Pair("ismine", IsSyscoinTxMine(txA, "offer") ? "true" : "false"));

		if(!ca.txBTCId.IsNull())
			oOfferAccept.push_back(Pair("paid","true(BTC)"));
		else
			oOfferAccept.push_back(Pair("paid","true"));
		CAliasIndex theAlias;
		CTransaction aliastx;
		bool isExpired = false;
		vector<CAliasIndex> aliasVtxPos;
		if(GetTxAndVtxOfAlias(acceptOffer.vchAlias, theAlias, aliastx, aliasVtxPos, isExpired, true))
		{
			theAlias.nHeight = acceptOffer.nHeight;
			theAlias.GetAliasFromList(aliasVtxPos);
		}
		string strMessage = string("");
		if(!DecryptMessage(theAlias.vchPubKey, ca.vchMessage, strMessage))
			strMessage = string("Encrypted for owner of offer");
		oOfferAccept.push_back(Pair("pay_message", strMessage));
		UniValue oBuyerFeedBack(UniValue::VARR);
		for(unsigned int j =0;j<buyerFeedBacks.size();j++)
		{
			UniValue oFeedback(UniValue::VOBJ);
			string sFeedbackTime;
			CBlockIndex *pindex = chainActive[buyerFeedBacks[j].nHeight];
			if (pindex) {
				sFeedbackTime = strprintf("%llu", pindex->nTime);
			}
			oFeedback.push_back(Pair("txid", buyerFeedBacks[j].txHash.GetHex()));
			oFeedback.push_back(Pair("time", sFeedbackTime));
			oFeedback.push_back(Pair("rating", buyerFeedBacks[j].nRating));
			oFeedback.push_back(Pair("feedbackuser", buyerFeedBacks[j].nFeedbackUser));
			oFeedback.push_back(Pair("feedback", stringFromVch(buyerFeedBacks[j].vchFeedback)));
			oBuyerFeedBack.push_back(oFeedback);
		}
		oOfferAccept.push_back(Pair("buyer_feedback", oBuyerFeedBack));
		oOfferAccept.push_back(Pair("avg_buyer_rating", avgBuyerRating));
		UniValue oSellerFeedBack(UniValue::VARR);
		for(unsigned int j =0;j<sellerFeedBacks.size();j++)
		{
			UniValue oFeedback(UniValue::VOBJ);
			string sFeedbackTime;
			CBlockIndex *pindex = chainActive[sellerFeedBacks[j].nHeight];
			if (pindex) {
				sFeedbackTime = strprintf("%llu", pindex->nTime);
			}
			oFeedback.push_back(Pair("txid", sellerFeedBacks[j].txHash.GetHex()));
			oFeedback.push_back(Pair("time", sFeedbackTime));
			oFeedback.push_back(Pair("rating", sellerFeedBacks[j].nRating));
			oFeedback.push_back(Pair("feedbackuser", sellerFeedBacks[j].nFeedbackUser));
			oFeedback.push_back(Pair("feedback", stringFromVch(sellerFeedBacks[j].vchFeedback)));
			oSellerFeedBack.push_back(oFeedback);
		}
		oOfferAccept.push_back(Pair("seller_feedback", oSellerFeedBack));
		oOfferAccept.push_back(Pair("avg_seller_rating", avgSellerRating));
		unsigned int ratingCount = 0;
		if(avgSellerRating > 0)
			ratingCount++;
		if(avgBuyerRating > 0)
			ratingCount++;
		if(ratingCount == 0)
			ratingCount = 1;
		float totalAvgRating = roundf((avgSellerRating+avgBuyerRating)/(float)ratingCount);
		oOfferAccept.push_back(Pair("avg_rating", (int)totalAvgRating));	

		aoOfferAccepts.push_back(oOfferAccept);
	}

	uint64_t nHeight;
	int expired;
	int expires_in;
	int expired_block;

	expired = 0;	
	expires_in = 0;
	expired_block = 0;
    nHeight = vtxPos.back().nHeight;
	vector<unsigned char> vchCert;
	if(!theOffer.vchCert.empty())
		vchCert = theOffer.vchCert;
	oOffer.push_back(Pair("offer", offer));
	oOffer.push_back(Pair("cert", stringFromVch(vchCert)));
	oOffer.push_back(Pair("txid", tx.GetHash().GetHex()));
	expired_block = nHeight + GetOfferExpirationDepth();
    if(expired_block < chainActive.Tip()->nHeight)
	{
		expired = 1;
	}  
	expires_in = expired_block - chainActive.Tip()->nHeight;
	oOffer.push_back(Pair("expires_in", expires_in));
	oOffer.push_back(Pair("expired_block", expired_block));
	oOffer.push_back(Pair("expired", expired));
	oOffer.push_back(Pair("height", strprintf("%llu", nHeight)));
	oOffer.push_back(Pair("category", stringFromVch(theOffer.sCategory)));
	oOffer.push_back(Pair("title", stringFromVch(theOffer.sTitle)));
	if(theOffer.nQty == -1)
		oOffer.push_back(Pair("quantity", "unlimited"));
	else
		oOffer.push_back(Pair("quantity", strprintf("%d", vtxPos.back().nQty)));
	oOffer.push_back(Pair("currency", stringFromVch(theOffer.sCurrencyCode)));
	
	
	int precision = 2;
	CAmount nPricePerUnit = convertCurrencyCodeToSyscoin(theOffer.vchAliasPeg, theOffer.sCurrencyCode, theOffer.GetPrice(), nHeight, precision);
	oOffer.push_back(Pair("sysprice", ValueFromAmount(nPricePerUnit)));
	oOffer.push_back(Pair("price", strprintf("%.*f", precision, theOffer.GetPrice() ))); 
	
	oOffer.push_back(Pair("ismine", IsSyscoinTxMine(tx, "offer") ? "true" : "false"));
	if(!theOffer.vchLinkOffer.empty()) {
		oOffer.push_back(Pair("commission", strprintf("%d%%", theOffer.nCommission)));
		oOffer.push_back(Pair("offerlink", "true"));
		oOffer.push_back(Pair("offerlink_guid", stringFromVch(theOffer.vchLinkOffer)));
		oOffer.push_back(Pair("offerlink_seller", stringFromVch(linkOffer.vchAlias)));

	}
	else
	{
		oOffer.push_back(Pair("commission", "0"));
		oOffer.push_back(Pair("offerlink", "false"));
		oOffer.push_back(Pair("offerlink_guid", ""));
		oOffer.push_back(Pair("offerlink_seller", ""));
	}
	oOffer.push_back(Pair("exclusive_resell", theOffer.linkWhitelist.bExclusiveResell ? "ON" : "OFF"));
	oOffer.push_back(Pair("private", theOffer.bPrivate ? "Yes" : "No"));
	oOffer.push_back(Pair("safesearch", theOffer.safeSearch ? "Yes" : "No"));
	oOffer.push_back(Pair("safetylevel", theOffer.safetyLevel ));
	oOffer.push_back(Pair("btconly", theOffer.bOnlyAcceptBTC ? "Yes" : "No"));
	oOffer.push_back(Pair("alias_peg", stringFromVch(theOffer.vchAliasPeg)));
	oOffer.push_back(Pair("description", stringFromVch(theOffer.sDescription)));
	oOffer.push_back(Pair("alias", stringFromVch(theOffer.vchAlias)));
	float rating = 0;
	if(vtxAliasPos.back().nRatingCount > 0)
		rating = roundf(vtxAliasPos.back().nRating/(float)vtxAliasPos.back().nRatingCount);
	oOffer.push_back(Pair("alias_rating",(int)rating));
	oOffer.push_back(Pair("geolocation", stringFromVch(theOffer.vchGeoLocation)));
	oOffer.push_back(Pair("offers_sold", (int)aoOfferAccepts.size()));
	oOffer.push_back(Pair("accepts", aoOfferAccepts));
	oLastOffer = oOffer;
	
	return oLastOffer;

}
UniValue offeracceptlist(const UniValue& params, bool fHelp) {
    if (fHelp || 1 < params.size())
		throw runtime_error("offeracceptlist [offer]\n"
				"list my offer accepts");

    vector<unsigned char> vchOffer;
	vector<unsigned char> vchOfferToFind;
    if (params.size() == 1)
        vchOfferToFind = vchFromValue(params[0]);	
	vector<unsigned char> vchEscrow;	
    map< vector<unsigned char>, int > vNamesI;
    UniValue oRes(UniValue::VARR);
    {

        uint256 blockHash;
        uint256 hash;
        CTransaction tx;
		
        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, pwalletMain->mapWallet)
        {
            // get txn hash, read txn index
            hash = item.second.GetHash();

 			const CWalletTx &wtx = item.second;
            // skip non-syscoin txns
            if (wtx.nVersion != SYSCOIN_TX_VERSION)
                continue;
			for (unsigned int j = 0; j < wtx.vout.size(); j++)
			{
				UniValue oOfferAccept(UniValue::VOBJ);
				// decode txn, skip non-alias txns
				vector<vector<unsigned char> > vvch;
				int op;
				if (!IsSyscoinScript(wtx.vout[j].scriptPubKey, op, vvch))
					continue;
				if(op != OP_OFFER_ACCEPT)
					continue;
				// dont show feedback outputs as accepts
				if(vvch[2] == vchFromString("1"))
					continue;

				if(vvch[0] != vchOfferToFind && !vchOfferToFind.empty())
					continue;
				vchOffer = vvch[0];
				
				COfferAccept theOfferAccept;

				// Check hash
				const vector<unsigned char> &vchAcceptRand = vvch[1];			
				CTransaction offerTx, acceptTx;
				COffer theOffer;
				if (!GetTxOfOfferAccept(vchOffer, vchAcceptRand, theOffer, theOfferAccept, acceptTx))
					continue;
				// get last active accepts only
				if (vNamesI.find(vchAcceptRand) != vNamesI.end() && (theOfferAccept.nHeight <= vNamesI[vchAcceptRand] || vNamesI[vchAcceptRand] < 0))
					continue;	
				string offer = stringFromVch(vchOffer);
				string sHeight = strprintf("%llu", theOfferAccept.nHeight);
				vector<COffer> vtxPos;
				if (!pofferdb->ReadOffer(vchOffer, vtxPos))
					continue;
				if (vtxPos.size() < 1)
					continue;
				bool isAcceptMine = IsSyscoinTxMine(acceptTx, "offer");
				bool skipAcceptBuyerSpecialOutput = false;
				int nOut = IndexOfOfferOutput(acceptTx, skipAcceptBuyerSpecialOutput);
				// if we own an output in this accept tx yet the tx isn't ours
				// it must be a buyer special output that was sent via linked accept
				// so we skip it from showing in the accept lists
				// this only applies to linked offer accepts because those ones don't attach buyer special output
				if(!theOffer.vchLinkOffer.empty())
				{
					if(nOut >= 0 && !isAcceptMine)
						continue;
				}
				oOfferAccept.push_back(Pair("offer", offer));
				oOfferAccept.push_back(Pair("title", stringFromVch(theOffer.sTitle)));
				oOfferAccept.push_back(Pair("id", stringFromVch(vchAcceptRand)));
				string strBTCId = "";
				if(!theOfferAccept.txBTCId.IsNull())
					strBTCId = theOfferAccept.txBTCId.GetHex();
				oOfferAccept.push_back(Pair("btctxid", strBTCId));
				oOfferAccept.push_back(Pair("alias", stringFromVch(theOffer.vchAlias)));
				oOfferAccept.push_back(Pair("buyer", stringFromVch(theOfferAccept.vchBuyerAlias)));
				oOfferAccept.push_back(Pair("height", sHeight));
				oOfferAccept.push_back(Pair("quantity", strprintf("%d", theOfferAccept.nQty)));
				oOfferAccept.push_back(Pair("currency", stringFromVch(theOffer.sCurrencyCode)));
				vector<unsigned char> vchOfferAcceptLink;
				bool foundOffer = false;
				for (unsigned int j = 0; j < acceptTx.vin.size(); j++) {
					vector<vector<unsigned char> > vvchIn;
					int opIn;
					const COutPoint *prevOutput = &acceptTx.vin[j].prevout;
					if(!GetPreviousInput(prevOutput, opIn, vvchIn))
						continue;
					if(foundOffer)
						break;

					if (!foundOffer && IsOfferOp(opIn)) {
						if(opIn == OP_OFFER_ACCEPT && vvchIn[2] != vchFromString("1"))
						{
							vchOfferAcceptLink = vvchIn[1];
							foundOffer = true; 
						}
					}
				}
				if(!theOfferAccept.vchEscrow.empty() && vchOfferAcceptLink.empty())
					continue;				
				string linkAccept = "";
				if(!vchOfferAcceptLink.empty())
					linkAccept = stringFromVch(vchOfferAcceptLink);
				oOfferAccept.push_back(Pair("linkofferaccept", linkAccept));
				if(!FindOfferAcceptPayment(acceptTx, theOfferAccept.nPrice) && theOfferAccept.txBTCId.IsNull())
					continue;
				oOfferAccept.push_back(Pair("offer_discount_percentage", strprintf("%.2f%%", 100.0f - 100.0f*(theOfferAccept.nPrice/theOffer.GetPrice()))));	
				int precision = 2;
				CAmount nPricePerUnit = convertCurrencyCodeToSyscoin(theOffer.vchAliasPeg, theOffer.sCurrencyCode, theOfferAccept.nPrice, theOfferAccept.nAcceptHeight, precision);
				oOfferAccept.push_back(Pair("systotal", ValueFromAmount(nPricePerUnit * theOfferAccept.nQty)));
				
				oOfferAccept.push_back(Pair("price", strprintf("%.*f", precision, theOffer.GetPrice() ))); 
				oOfferAccept.push_back(Pair("total", strprintf("%.*f", precision, theOfferAccept.nPrice * theOfferAccept.nQty ))); 
				// this accept is for me(something ive sold) if this offer is mine
				oOfferAccept.push_back(Pair("ismine", isAcceptMine? "true" : "false"));

				if(!theOfferAccept.txBTCId.IsNull())
					oOfferAccept.push_back(Pair("status","paid(BTC)"));
				else
					oOfferAccept.push_back(Pair("status","paid"));

				CAliasIndex theAlias;
				CTransaction aliastx;
				bool isExpired = false;
				vector<CAliasIndex> aliasVtxPos;
				if(GetTxAndVtxOfAlias(theOffer.vchAlias, theAlias, aliastx, aliasVtxPos, isExpired, true))
				{
					theAlias.nHeight = theOffer.nHeight;
					theAlias.GetAliasFromList(aliasVtxPos);
				}
				string strMessage = string("");
				if(!DecryptMessage(theAlias.vchPubKey, theOfferAccept.vchMessage, strMessage))
					strMessage = string("Encrypted for owner of offer");
				oOfferAccept.push_back(Pair("pay_message", strMessage));
				oRes.push_back(oOfferAccept);
				vNamesI[vchAcceptRand] = theOfferAccept.nHeight;
			}
        }
	}

    return oRes;
}
UniValue offerlist(const UniValue& params, bool fHelp) {
    if (fHelp || 1 < params.size())
		throw runtime_error("offerlist [offer]\n"
				"list my own offers");

    vector<unsigned char> vchOffer;

    if (params.size() == 1)
        vchOffer = vchFromValue(params[0]);

    vector<unsigned char> vchNameUniq;
    if (params.size() == 1)
        vchNameUniq = vchFromValue(params[0]);

    UniValue oRes(UniValue::VARR);
    map< vector<unsigned char>, int > vNamesI;
    map< vector<unsigned char>, UniValue > vNamesO;

    {
		int nQty = 0;
        uint256 blockHash;
        uint256 hash;
        CTransaction tx;
		int expired;
		int pending;
		int expires_in;
		int expired_block;
        uint64_t nHeight;

        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, pwalletMain->mapWallet)
        {
			expired = 0;
			pending = 0;
			expires_in = 0;
			expired_block = 0;
            // get txn hash, read txn index
            hash = item.second.GetHash();

 			const CWalletTx &wtx = item.second;

            // skip non-syscoin txns
            if (wtx.nVersion != SYSCOIN_TX_VERSION)
                continue;

            // decode txn, skip non-alias txns
            vector<vector<unsigned char> > vvch;
            int op, nOut;
            if (!DecodeOfferTx(wtx, op, nOut, vvch) 
            	|| !IsOfferOp(op) 
            	|| (op == OP_OFFER_ACCEPT))
                continue;

            // get the txn name
            vchOffer = vvch[0];

			// skip this offer if it doesn't match the given filter value
			if (vchNameUniq.size() > 0 && vchNameUniq != vchOffer)
				continue;
	
			vector<COffer> vtxPos;
			COffer theOfferA;
			if (!pofferdb->ReadOffer(vchOffer, vtxPos) || vtxPos.empty())
			{
				pending = 1;
				theOfferA = COffer(wtx);
				if(!IsSyscoinTxMine(wtx, "offer"))
					continue;
			}	
			else
			{
				nQty = vtxPos.back().nQty;
				CTransaction tx;
				if(!GetTxOfOffer( vchOffer, theOfferA, tx))
				{
					pending = 1;
					if(!IsSyscoinTxMine(wtx, "offer"))
						continue;
				}
				else
				{
					if (!DecodeOfferTx(tx, op, nOut, vvch) || !IsOfferOp(op))
						continue;
					if(!IsSyscoinTxMine(tx, "offer"))
						continue;
				}
			}	
			// get last active name only
			if (vNamesI.find(vchOffer) != vNamesI.end() && (theOfferA.nHeight <= vNamesI[vchOffer] || vNamesI[vchOffer] < 0))
				continue;	
			nHeight = theOfferA.nHeight;
            // build the output UniValue
            UniValue oName(UniValue::VOBJ);
            oName.push_back(Pair("offer", stringFromVch(vchOffer)));
			vector<unsigned char> vchCert;
			if(!theOfferA.vchCert.empty())
				vchCert = theOfferA.vchCert;
			oName.push_back(Pair("cert", stringFromVch(vchCert)));
            oName.push_back(Pair("title", stringFromVch(theOfferA.sTitle)));
            oName.push_back(Pair("category", stringFromVch(theOfferA.sCategory)));
            oName.push_back(Pair("description", stringFromVch(theOfferA.sDescription)));
			int precision = 2;
			convertCurrencyCodeToSyscoin(theOfferA.vchAliasPeg, theOfferA.sCurrencyCode, 0, chainActive.Tip()->nHeight, precision);
			oName.push_back(Pair("price", strprintf("%.*f", precision, theOfferA.GetPrice() ))); 	

			oName.push_back(Pair("currency", stringFromVch(theOfferA.sCurrencyCode) ) );
			oName.push_back(Pair("commission", strprintf("%d%%", theOfferA.nCommission)));
			if(nQty == -1)
				oName.push_back(Pair("quantity", "unlimited"));
			else
				oName.push_back(Pair("quantity", strprintf("%d", nQty)));
			vector<CAliasIndex> vtxAliasPos;
			paliasdb->ReadAlias(theOfferA.vchAlias, vtxAliasPos);
				
			oName.push_back(Pair("exclusive_resell", theOfferA.linkWhitelist.bExclusiveResell ? "ON" : "OFF"));
			oName.push_back(Pair("btconly", theOfferA.bOnlyAcceptBTC ? "Yes" : "No"));
			oName.push_back(Pair("alias_peg", stringFromVch(theOfferA.vchAliasPeg)));
			oName.push_back(Pair("private", theOfferA.bPrivate ? "Yes" : "No"));
			oName.push_back(Pair("safesearch", theOfferA.safeSearch ? "Yes" : "No"));
			oName.push_back(Pair("safetylevel", theOfferA.safetyLevel ));
			oName.push_back(Pair("geolocation", stringFromVch(theOfferA.vchGeoLocation)));
			oName.push_back(Pair("offers_sold", GetNumberOfAccepts(vtxPos)));
			expired_block = nHeight + GetOfferExpirationDepth();
            if(expired_block < chainActive.Tip()->nHeight)
			{
				expired = 1;
			}  
			expires_in = expired_block - chainActive.Tip()->nHeight;
			
			oName.push_back(Pair("alias", stringFromVch(theOfferA.vchAlias)));
			float rating = 0;
			if(!vtxAliasPos.empty() && vtxAliasPos.back().nRatingCount > 0)
				rating = roundf(vtxAliasPos.back().nRating/(float)vtxAliasPos.back().nRatingCount);
			oName.push_back(Pair("alias_rating",(int)rating));
			oName.push_back(Pair("expires_in", expires_in));
			oName.push_back(Pair("expires_on", expired_block));
			oName.push_back(Pair("expired", expired));

			oName.push_back(Pair("pending", pending));

            vNamesI[vchOffer] = nHeight;
            vNamesO[vchOffer] = oName;
        }
    }

    BOOST_FOREACH(const PAIRTYPE(vector<unsigned char>, UniValue)& item, vNamesO)
        oRes.push_back(item.second);

    return oRes;
}


UniValue offerhistory(const UniValue& params, bool fHelp) {
	if (fHelp || 1 != params.size())
		throw runtime_error("offerhistory <offer>\n"
				"List all stored values of an offer.\n");

	UniValue oRes(UniValue::VARR);
	vector<unsigned char> vchOffer = vchFromValue(params[0]);
	string offer = stringFromVch(vchOffer);

	{

		vector<COffer> vtxPos;
		if (!pofferdb->ReadOffer(vchOffer, vtxPos) || vtxPos.empty())
			throw runtime_error("failed to read from offer DB");

		COffer txPos2;
		uint256 txHash;
		BOOST_FOREACH(txPos2, vtxPos) {
			txHash = txPos2.txHash;
			CTransaction tx;
			if (!GetSyscoinTransaction(txPos2.nHeight, txHash, tx, Params().GetConsensus())) {
				error("could not read txpos");
				continue;
			}
            // decode txn, skip non-alias txns
            vector<vector<unsigned char> > vvch;
            int op, nOut;
            if (!DecodeOfferTx(tx, op, nOut, vvch) 
            	|| !IsOfferOp(op) )
                continue;

			int expired = 0;
			int expires_in = 0;
			int expired_block = 0;
			UniValue oOffer(UniValue::VOBJ);
			vector<unsigned char> vchValue;
			uint64_t nHeight;
			nHeight = txPos2.nHeight;
			COffer theOfferA = txPos2;
			oOffer.push_back(Pair("offer", offer));
			string opName = offerFromOp(op);
			oOffer.push_back(Pair("offertype", opName));
			vector<unsigned char> vchCert;
			if(!theOfferA.vchCert.empty())
				vchCert = theOfferA.vchCert;
			oOffer.push_back(Pair("cert", stringFromVch(vchCert)));
            oOffer.push_back(Pair("title", stringFromVch(theOfferA.sTitle)));
            oOffer.push_back(Pair("category", stringFromVch(theOfferA.sCategory)));
            oOffer.push_back(Pair("description", stringFromVch(theOfferA.sDescription)));
			int precision = 2;
			convertCurrencyCodeToSyscoin(theOfferA.vchAliasPeg, theOfferA.sCurrencyCode, 0, chainActive.Tip()->nHeight, precision);
			oOffer.push_back(Pair("price", strprintf("%.*f", precision, theOfferA.GetPrice() ))); 	

			oOffer.push_back(Pair("currency", stringFromVch(theOfferA.sCurrencyCode) ) );
			oOffer.push_back(Pair("commission", strprintf("%d%%", theOfferA.nCommission)));
			if(theOfferA.nQty == -1)
				oOffer.push_back(Pair("quantity", "unlimited"));
			else
				oOffer.push_back(Pair("quantity", strprintf("%d", theOfferA.nQty)));

			oOffer.push_back(Pair("txid", tx.GetHash().GetHex()));
			expired_block = nHeight + GetOfferExpirationDepth();
            if(expired_block < chainActive.Tip()->nHeight)
			{
				expired = 1;
			}  
			expires_in = expired_block - chainActive.Tip()->nHeight;
	
			vector<CAliasIndex> vtxAliasPos;
			paliasdb->ReadAlias(theOfferA.vchAlias, vtxAliasPos);
			oOffer.push_back(Pair("alias", stringFromVch(theOfferA.vchAlias)));
			float rating = 0;
			if(!vtxAliasPos.empty() && vtxAliasPos.back().nRatingCount > 0)
				rating = roundf(vtxAliasPos.back().nRating/(float)vtxAliasPos.back().nRatingCount);
			oOffer.push_back(Pair("alias_rating",(int)rating));
			oOffer.push_back(Pair("expires_in", expires_in));
			oOffer.push_back(Pair("expires_on", expired_block));
			oOffer.push_back(Pair("expired", expired));
			oOffer.push_back(Pair("height", strprintf("%d", theOfferA.nHeight)));
			oRes.push_back(oOffer);
		}
	}
	return oRes;
}

UniValue offerfilter(const UniValue& params, bool fHelp) {
	if (fHelp || params.size() > 4)
		throw runtime_error(
				"offerfilter [[[[[regexp]] from=0]] safesearch='Yes' category]\n"
						"scan and filter offers\n"
						"[regexp] : apply [regexp] on offers, empty means all offers\n"
						"[from] : show results from this GUID [from], 0 means first.\n"
						"[safesearch] : shows all offers that are safe to display (not on the ban list)\n"
						"[category] : category you want to search in, empty for all\n"
						"offerfilter \"\" 5 # list offers updated in last 5 blocks\n"
						"offerfilter \"^offer\" # list all offers starting with \"offer\"\n"
						"offerfilter 36000 0 0 stat # display stats (number of offers) on active offers\n");

	string strRegexp;
	vector<unsigned char> vchOffer;
	string strCategory;
	bool safeSearch = true;

	if (params.size() > 0)
		strRegexp = params[0].get_str();

	if (params.size() > 1)
		vchOffer = vchFromValue(params[1]);

	if (params.size() > 2)
		safeSearch = params[2].get_str()=="On"? true: false;

	if (params.size() > 3)
		strCategory = params[3].get_str();

	UniValue oRes(UniValue::VARR);

	
	vector<pair<vector<unsigned char>, COffer> > offerScan;
	if (!pofferdb->ScanOffers(vchOffer, strRegexp, safeSearch, strCategory, 25, offerScan))
		throw runtime_error("scan failed");
	
	pair<vector<unsigned char>, COffer> pairScan;
	BOOST_FOREACH(pairScan, offerScan) {
		const COffer &txOffer = pairScan.second;
		const string &offer = stringFromVch(pairScan.first);
		vector<COffer> vtxPos;
		vector<CAliasIndex> vtxAliasPos;
		if (!pofferdb->ReadOffer(vchFromString(offer), vtxPos) || vtxPos.empty())
			continue;

		paliasdb->ReadAlias(txOffer.vchAlias, vtxAliasPos);
		int expired = 0;
		int expires_in = 0;
		int expired_block = 0;		
		int nHeight = txOffer.nHeight;
		UniValue oOffer(UniValue::VOBJ);
		oOffer.push_back(Pair("offer", offer));
		vector<unsigned char> vchCert;
		if(!txOffer.vchCert.empty())
			vchCert = txOffer.vchCert;
		oOffer.push_back(Pair("cert", stringFromVch(vchCert)));
        oOffer.push_back(Pair("title", stringFromVch(txOffer.sTitle)));
		oOffer.push_back(Pair("description", stringFromVch(txOffer.sDescription)));
        oOffer.push_back(Pair("category", stringFromVch(txOffer.sCategory)));
		int precision = 2;
		convertCurrencyCodeToSyscoin(txOffer.vchAliasPeg, txOffer.sCurrencyCode, 0, chainActive.Tip()->nHeight, precision);
		COffer foundOffer = txOffer;	
		oOffer.push_back(Pair("price", strprintf("%.*f", precision, foundOffer.GetPrice() ))); 	
		oOffer.push_back(Pair("currency", stringFromVch(txOffer.sCurrencyCode)));
		oOffer.push_back(Pair("commission", strprintf("%d%%", txOffer.nCommission)));
		if(txOffer.nQty == -1)
			oOffer.push_back(Pair("quantity", "unlimited"));
		else
			oOffer.push_back(Pair("quantity", strprintf("%d", txOffer.nQty)));
		oOffer.push_back(Pair("exclusive_resell", txOffer.linkWhitelist.bExclusiveResell ? "ON" : "OFF"));
		oOffer.push_back(Pair("btconly", txOffer.bOnlyAcceptBTC ? "Yes" : "No"));
		oOffer.push_back(Pair("alias_peg", stringFromVch(txOffer.vchAliasPeg)));
		oOffer.push_back(Pair("offers_sold", GetNumberOfAccepts(vtxPos)));
		expired_block = nHeight + GetOfferExpirationDepth();  
		expires_in = expired_block - chainActive.Tip()->nHeight;
		oOffer.push_back(Pair("private", txOffer.bPrivate ? "Yes" : "No"));
		oOffer.push_back(Pair("alias", stringFromVch(txOffer.vchAlias)));
		float rating = 0;
		if(!vtxAliasPos.empty() && vtxAliasPos.back().nRatingCount > 0)
			rating = roundf(vtxAliasPos.back().nRating/(float)vtxAliasPos.back().nRatingCount);
		oOffer.push_back(Pair("alias_rating",(int)rating));
		oOffer.push_back(Pair("geolocation", stringFromVch(txOffer.vchGeoLocation)));
		oOffer.push_back(Pair("expires_in", expires_in));
		oOffer.push_back(Pair("expires_on", expired_block));
		oRes.push_back(oOffer);
	}


	return oRes;
}
int GetNumberOfAccepts(const std::vector<COffer> &offerList) {
	int count = 0;
	for(unsigned int i =0;i<offerList.size();i++) {
		if(!offerList[i].accept.IsNull() && offerList[i].accept.feedback.IsNull())
			count++;
    }
    return count;
}
bool GetAcceptByHash(std::vector<COffer> &offerList, COfferAccept &ca, bool skipFeedback) {
	if(offerList.empty())
		return false;
	for(std::vector<COffer>::reverse_iterator it = offerList.rbegin(); it != offerList.rend(); ++it) {
		const COffer& myoffer = *it;
		// skip null states or ones with feedback (these aren't considered normal accepts)
		if(myoffer.accept.IsNull() || (skipFeedback && !myoffer.accept.feedback.IsNull()))
			continue;
        if(myoffer.accept.vchAcceptRand == ca.vchAcceptRand) {
            ca = myoffer.accept;
			return true;
        }
    }
    ca = offerList.back().accept;
	return false;
}