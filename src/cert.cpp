#include "cert.h"
#include "alias.h"
#include "offer.h"
#include "init.h"
#include "main.h"
#include "util.h"
#include "random.h"
#include "base58.h"
#include "core_io.h"
#include "rpc/server.h"
#include "wallet/wallet.h"
#include "chainparams.h"
#include "messagecrypter.h"
#include <boost/algorithm/hex.hpp>
#include <boost/algorithm/string/case_conv.hpp> // for to_lower()
#include <boost/xpressive/xpressive_dynamic.hpp>
#include <boost/foreach.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/thread.hpp>
#include <boost/algorithm/string/predicate.hpp>
using namespace std;
extern void SendMoneySyscoin(const vector<CRecipient> &vecSend, CAmount nValue, bool fSubtractFeeFromAmount, CWalletTx& wtxNew, const CWalletTx* wtxInAlias=NULL, bool syscoinMultiSigTx=false);
bool EncryptMessage(const vector<unsigned char> &vchPubKey, const vector<unsigned char> &vchMessage, string &strCipherText)
{
	CMessageCrypter crypter;
	if(!crypter.Encrypt(stringFromVch(vchPubKey), stringFromVch(vchMessage), strCipherText))
		return false;

	return true;
}
bool DecryptMessage(const vector<unsigned char> &vchPubKey, const vector<unsigned char> &vchCipherText, string &strMessage)
{
	CKey PrivateKey;
	CPubKey PubKey(vchPubKey);
	CKeyID pubKeyID = PubKey.GetID();
	if (!pwalletMain->GetKey(pubKeyID, PrivateKey))
        return false;
	CSyscoinSecret Secret(PrivateKey);
	PrivateKey = Secret.GetKey();
	std::vector<unsigned char> vchPrivateKey(PrivateKey.begin(), PrivateKey.end());
	CMessageCrypter crypter;
	if(!crypter.Decrypt(stringFromVch(vchPrivateKey), stringFromVch(vchCipherText), strMessage))
		return false;
	
	return true;
}
void PutToCertList(std::vector<CCert> &certList, CCert& index) {
	int i = certList.size() - 1;
	BOOST_REVERSE_FOREACH(CCert &o, certList) {
        if(index.nHeight != 0 && o.nHeight == index.nHeight) {
        	certList[i] = index;
            return;
        }
        else if(!o.txHash.IsNull() && o.txHash == index.txHash) {
        	certList[i] = index;
            return;
        }
        i--;
	}
    certList.push_back(index);
}
bool IsCertOp(int op) {
    return op == OP_CERT_ACTIVATE
        || op == OP_CERT_UPDATE
        || op == OP_CERT_TRANSFER;
}

// Increase expiration to 36000 gradually starting at block 24000.
// Use for validation purposes and pass the chain height.
int GetCertExpirationDepth() {
	#ifdef ENABLE_DEBUGRPC
    return 1440;
  #else
    return 525600;
  #endif
}


string certFromOp(int op) {
    switch (op) {
    case OP_CERT_ACTIVATE:
        return "certactivate";
    case OP_CERT_UPDATE:
        return "certupdate";
    case OP_CERT_TRANSFER:
        return "certtransfer";
    default:
        return "<unknown cert op>";
    }
}
bool CCert::UnserializeFromData(const vector<unsigned char> &vchData, const vector<unsigned char> &vchHash) {
    try {
        CDataStream dsCert(vchData, SER_NETWORK, PROTOCOL_VERSION);
        dsCert >> *this;

		const vector<unsigned char> &vchCertData = Serialize();
		uint256 calculatedHash = Hash(vchCertData.begin(), vchCertData.end());
		vector<unsigned char> vchRand = CScriptNum(calculatedHash.GetCheapHash()).getvch();
		vector<unsigned char> vchRandCert = vchFromValue(HexStr(vchRand));
		if(vchRandCert != vchHash)
		{
			SetNull();
			return false;
		}

    } catch (std::exception &e) {
		SetNull();
        return false;
    }
	return true;
}
bool CCert::UnserializeFromTx(const CTransaction &tx) {
	vector<unsigned char> vchData;
	vector<unsigned char> vchHash;
	int nOut;
	if(!GetSyscoinData(tx, vchData, vchHash, nOut))
	{
		SetNull();
		return false;
	}
	if(!UnserializeFromData(vchData, vchHash))
	{	
		return false;
	}
    return true;
}
const vector<unsigned char> CCert::Serialize() {
    CDataStream dsCert(SER_NETWORK, PROTOCOL_VERSION);
    dsCert << *this;
    const vector<unsigned char> vchData(dsCert.begin(), dsCert.end());
    return vchData;

}
bool CCertDB::ScanCerts(const std::vector<unsigned char>& vchCert, const string &strRegexp, bool safeSearch, const string& strCategory, unsigned int nMax,
        std::vector<std::pair<std::vector<unsigned char>, CCert> >& certScan) {
    // regexp
    using namespace boost::xpressive;
    smatch certparts;
	string strRegexpLower = strRegexp;
	boost::algorithm::to_lower(strRegexpLower);
    sregex cregex = sregex::compile(strRegexpLower);
	int nMaxAge  = GetCertExpirationDepth();
	boost::scoped_ptr<CDBIterator> pcursor(NewIterator());
	pcursor->Seek(make_pair(string("certi"), vchCert));
    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
		pair<string, vector<unsigned char> > key;
        try {
			if (pcursor->GetKey(key) && key.first == "certi") {
            	vector<unsigned char> vchCert = key.second;
                vector<CCert> vtxPos;
				pcursor->GetValue(vtxPos);
				if (vtxPos.empty()){
					pcursor->Next();
					continue;
				}
				const CCert &txPos = vtxPos.back();
  				if (chainActive.Tip()->nHeight - txPos.nHeight >= nMaxAge)
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
				const string &cert = stringFromVch(vchCert);
				string title = stringFromVch(txPos.vchTitle);
				boost::algorithm::to_lower(title);
				if (strRegexp != "" && !regex_search(title, certparts, cregex) && strRegexp != cert && strRegexpLower != stringFromVch(txPos.vchAlias))
				{
					pcursor->Next();
					continue;
				}
				certScan.push_back(make_pair(vchCert, txPos));
			}
			if (certScan.size() >= nMax)
				break;

			pcursor->Next();
        } catch (std::exception &e) {
            return error("%s() : deserialize error", __PRETTY_FUNCTION__);
        }
    }
    return true;
}

int IndexOfCertOutput(const CTransaction& tx) {
	if (tx.nVersion != SYSCOIN_TX_VERSION)
		return -1;
    vector<vector<unsigned char> > vvch;
	int op;
	for (unsigned int i = 0; i < tx.vout.size(); i++) {
		const CTxOut& out = tx.vout[i];
		// find an output you own
		if (pwalletMain->IsMine(out) && DecodeCertScript(out.scriptPubKey, op, vvch)) {
			return i;
		}
	}
	return -1;
}

bool GetTxOfCert(const vector<unsigned char> &vchCert,
        CCert& txPos, CTransaction& tx, bool skipExpiresCheck) {
    vector<CCert> vtxPos;
    if (!pcertdb->ReadCert(vchCert, vtxPos) || vtxPos.empty())
        return false;
    txPos = vtxPos.back();
    int nHeight = txPos.nHeight;
    if (!skipExpiresCheck && (nHeight + GetCertExpirationDepth()
            < chainActive.Tip()->nHeight)) {
        string cert = stringFromVch(vchCert);
        LogPrintf("GetTxOfCert(%s) : expired", cert.c_str());
        return false;
    }

    if (!GetSyscoinTransaction(nHeight, txPos.txHash, tx, Params().GetConsensus()))
        return error("GetTxOfCert() : could not read tx from disk");

    return true;
}

bool GetTxAndVtxOfCert(const vector<unsigned char> &vchCert,
        CCert& txPos, CTransaction& tx,  vector<CCert> &vtxPos, bool skipExpiresCheck) {
    if (!pcertdb->ReadCert(vchCert, vtxPos) || vtxPos.empty())
        return false;
    txPos = vtxPos.back();
    int nHeight = txPos.nHeight;
    if (!skipExpiresCheck && (nHeight + GetCertExpirationDepth()
            < chainActive.Tip()->nHeight)) {
        string cert = stringFromVch(vchCert);
        LogPrintf("GetTxOfCert(%s) : expired", cert.c_str());
        return false;
    }

    if (!GetSyscoinTransaction(nHeight, txPos.txHash, tx, Params().GetConsensus()))
        return error("GetTxOfCert() : could not read tx from disk");

    return true;
}
bool DecodeAndParseCertTx(const CTransaction& tx, int& op, int& nOut,
		vector<vector<unsigned char> >& vvch)
{
	CCert cert;
	bool decode = DecodeCertTx(tx, op, nOut, vvch);
	bool parse = cert.UnserializeFromTx(tx);
	return decode && parse;
}
bool DecodeCertTx(const CTransaction& tx, int& op, int& nOut,
        vector<vector<unsigned char> >& vvch) {
    bool found = false;


    // Strict check - bug disallowed
    for (unsigned int i = 0; i < tx.vout.size(); i++) {
        const CTxOut& out = tx.vout[i];
        vector<vector<unsigned char> > vvchRead;
        if (DecodeCertScript(out.scriptPubKey, op, vvchRead)) {
            nOut = i; found = true; vvch = vvchRead;
            break;
        }
    }
    if (!found) vvch.clear();
    return found;
}


bool DecodeCertScript(const CScript& script, int& op,
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
    return IsCertOp(op);
}
bool DecodeCertScript(const CScript& script, int& op,
        vector<vector<unsigned char> > &vvch) {
    CScript::const_iterator pc = script.begin();
    return DecodeCertScript(script, op, vvch, pc);
}
CScript RemoveCertScriptPrefix(const CScript& scriptIn) {
    int op;
    vector<vector<unsigned char> > vvch;
    CScript::const_iterator pc = scriptIn.begin();

    if (!DecodeCertScript(scriptIn, op, vvch, pc))
        throw runtime_error(
                "RemoveCertScriptPrefix() : could not decode cert script");
	
    return CScript(pc, scriptIn.end());
}

bool CheckCertInputs(const CTransaction &tx, int op, int nOut, const vector<vector<unsigned char> > &vvchArgs,
        const CCoinsViewCache &inputs, bool fJustCheck, int nHeight, string &errorMessage, const CBlock* block, bool dontaddtodb) {
	if(!IsSys21Fork(nHeight))
		return true;	
	if (tx.IsCoinBase())
		return true;
	if (fDebug)
		LogPrintf("*** CERT %d %d %s %s\n", nHeight,
			chainActive.Tip()->nHeight, tx.GetHash().ToString().c_str(),
			fJustCheck ? "JUSTCHECK" : "BLOCK");
	bool foundAlias = false;
    const COutPoint *prevOutput = NULL;
    CCoins prevCoins;

	int prevAliasOp = 0;
    // Make sure cert outputs are not spent by a regular transaction, or the cert would be lost
	if (tx.nVersion != SYSCOIN_TX_VERSION) 
	{
		errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2000 - " + _("Non-Syscoin transaction found");
		return true;
	}
	vector<vector<unsigned char> > vvchPrevAliasArgs;
	// unserialize cert from txn, check for valid
	CCert theCert;
	vector<unsigned char> vchData;
	bool found = false;
	vector<unsigned char> vchHash;
	int nDataOut;
	if(!GetSyscoinData(tx, vchData, vchHash, nDataOut) || !theCert.UnserializeFromData(vchData, vchHash))
	{
		errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR ERRCODE: 2001 - " + _("Cannot unserialize data inside of this transaction relating to a certificate");
		return true;
	}

	if(fJustCheck)
	{
		if(!vchData.empty())
		{
			CRecipient fee;
			CScript scriptData;
			scriptData << vchData;
			CreateFeeRecipient(scriptData, vchData, fee);
			if (fee.nAmount > tx.vout[nDataOut].nValue) 
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2002 - " + _("Transaction does not pay enough fees");
				return error(errorMessage.c_str());
			}
		}
		if(vvchArgs.size() != 2)
		{
			errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2003 - " + _("Certificate arguments incorrect size");
			return error(errorMessage.c_str());
		}

					
		if(vvchArgs.size() <= 1 || vchHash != vvchArgs[1])
		{
			errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2004 - " + _("Hash provided doesn't match the calculated hash the data");
			return error(errorMessage.c_str());
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
			if(foundAlias)
				break;
			else if (!foundAlias && IsAliasOp(pop))
			{
				foundAlias = true; 
				prevAliasOp = pop;
				vvchPrevAliasArgs = vvch;
			}
		}
	}


	
	CAliasIndex alias;
	CTransaction aliasTx;
	vector<CCert> vtxPos;
	string retError = "";
	if(fJustCheck)
	{
		if (vvchArgs.empty() ||  vvchArgs[0].size() > MAX_GUID_LENGTH)
		{
			errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2006 - " + _("Certificate hex guid too long");
			return error(errorMessage.c_str());
		}
		if(theCert.sCategory.size() > MAX_NAME_LENGTH)
		{
			errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2007 - " + _("Certificate category too big");
			return error(errorMessage.c_str());
		}
		if(theCert.vchData.size() > MAX_ENCRYPTED_VALUE_LENGTH)
		{
			errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2008 - " + _("Certificate data too big");
			return error(errorMessage.c_str());
		}
		if(!theCert.vchCert.empty() && theCert.vchCert != vvchArgs[0])
		{
			errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2009 - " + _("Guid in data output doesn't match guid in transaction");
			return error(errorMessage.c_str());
		}
		switch (op) {
		case OP_CERT_ACTIVATE:
			if (theCert.vchCert != vvchArgs[0])
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2011 - " + _("Certificate guid mismatch");
				return error(errorMessage.c_str());
			}
			if(!theCert.vchLinkAlias.empty())
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2012 - " + _("Certificate linked alias not allowed in activate");
				return error(errorMessage.c_str());
			}
			if(!IsAliasOp(prevAliasOp) || theCert.vchAlias != vvchPrevAliasArgs[0])
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2013 - " + _("Alias input mismatch");
				return error(errorMessage.c_str());
			}
			if((theCert.vchTitle.size() > MAX_NAME_LENGTH || theCert.vchTitle.empty()))
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2014 - " + _("Certificate title too big or is empty");
				return error(errorMessage.c_str());
			}
			break;

		case OP_CERT_UPDATE:
			if (theCert.vchCert != vvchArgs[0])
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2018 - " + _("Certificate guid mismatch");
				return error(errorMessage.c_str());
			}
			if(theCert.vchTitle.size() > MAX_NAME_LENGTH)
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2019 - " + _("Certificate title too big");
				return error(errorMessage.c_str());
			}
			if(!IsAliasOp(prevAliasOp) || theCert.vchAlias != vvchPrevAliasArgs[0])
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2020 - " + _("Alias input mismatch");
				return error(errorMessage.c_str());
			}
			break;

		case OP_CERT_TRANSFER:
			if (theCert.vchCert != vvchArgs[0])
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2023 - " + _("Certificate guid mismatch");
				return error(errorMessage.c_str());
			}
			if(!IsAliasOp(prevAliasOp) || theCert.vchAlias != vvchPrevAliasArgs[0])
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2024 - " + _("Alias input mismatch");
				return error(errorMessage.c_str());
			}
			break;

		default:
			errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2025 - " + _("Certificate transaction has unknown op");
			return error(errorMessage.c_str());
		}
	}

    if (!fJustCheck ) {
		if(op != OP_CERT_ACTIVATE) 
		{
			// if not an certnew, load the cert data from the DB
			CTransaction certTx;
			CCert dbCert;

			if(!GetTxAndVtxOfCert(vvchArgs[0], dbCert, certTx, vtxPos))	
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2026 - " + _("Failed to read from certificate DB");
				return true;
			}
			if(theCert.vchData.empty())
				theCert.vchData = dbCert.vchData;
			if(theCert.vchTitle.empty())
				theCert.vchTitle = dbCert.vchTitle;
			if(theCert.sCategory.empty())
				theCert.sCategory = dbCert.sCategory;
			// user can't update safety level after creation
			theCert.safetyLevel = dbCert.safetyLevel;
			theCert.vchCert = dbCert.vchCert;
			if(theCert.vchAlias != dbCert.vchAlias)
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 1063 - " + _("Wrong alias input provided in this certificate transaction");
				theCert.vchAlias = dbCert.vchAlias;
			}
			else if(!theCert.vchLinkAlias.empty())
				theCert.vchAlias = theCert.vchLinkAlias;

			if(op == OP_CERT_TRANSFER)
			{
				// check to alias
				if(!GetTxOfAlias(theCert.vchLinkAlias, alias, aliasTx))
				{
					errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2027 - " + _("Cannot find alias you are transfering to. It may be expired");
					theCert.vchAlias = dbCert.vchAlias;						
				}
				else
				{
							
					if(!alias.acceptCertTransfers)
					{
						errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2028 - " + _("The alias you are transferring to does not accept certificate transfers");
						theCert.vchAlias = dbCert.vchAlias;	
					}
				}
			}
			theCert.vchLinkAlias.clear();
			if(!GetTxOfAlias(theCert.vchAlias, alias, aliasTx))
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2029 - " + _("Cannot find alias for this certificate. It may be expired");	
				theCert = dbCert;
			}
		}
		else
		{
			if (!GetTxOfAlias(theCert.vchAlias, alias, aliasTx))
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2030 - " + _("Cannot find alias for this certificate. It may be expired");
				return true;
			}
			if (pcertdb->ExistsCert(vvchArgs[0]))
			{
				errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2031 - " + _("Certificate already exists");
				return true;
			}
		}
        // set the cert's txn-dependent values
		theCert.nHeight = nHeight;
		theCert.txHash = tx.GetHash();
		PutToCertList(vtxPos, theCert);
        // write cert  

        if (!dontaddtodb && !pcertdb->WriteCert(vvchArgs[0], vtxPos))
		{
			errorMessage = "SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2032 - " + _("Failed to write to certifcate DB");
            return error(errorMessage.c_str());
		}
		

      			
        // debug
		if(fDebug)
			LogPrintf( "CONNECTED CERT: op=%s cert=%s title=%s hash=%s height=%d\n",
                certFromOp(op).c_str(),
                stringFromVch(vvchArgs[0]).c_str(),
                stringFromVch(theCert.vchTitle).c_str(),
                tx.GetHash().ToString().c_str(),
                nHeight);
    }
    return true;
}





UniValue certnew(const UniValue& params, bool fHelp) {
    if (fHelp || params.size() < 3 || params.size() > 6)
        throw runtime_error(
		"certnew <alias> <title> <data> [private=0] [safe search=Yes] [category=certificates]\n"
						"<alias> An alias you own.\n"
                        "<title> title, 255 bytes max.\n"
                        "<data> data, 1KB max.\n"
						"<private> set to 1 if you only want to make the cert data private, only the owner of the cert can view it. Off by default.\n"
 						"<safe search> set to No if this cert should only show in the search when safe search is not selected. Defaults to Yes (cert shows with or without safe search selected in search lists).\n"                     
						"<category> category, 255 chars max. Defaults to certificates\n"
						+ HelpRequiringPassphrase());
	vector<unsigned char> vchAlias = vchFromValue(params[0]);
	vector<unsigned char> vchTitle = vchFromString(params[1].get_str());
    vector<unsigned char> vchData = vchFromString(params[2].get_str());
	vector<unsigned char> vchCat = vchFromString("certificates");
	// check for alias existence in DB
	CTransaction aliastx;
	CAliasIndex theAlias;
	const CWalletTx *wtxAliasIn = NULL;
	if (!GetTxOfAlias(vchAlias, theAlias, aliastx, true))
		throw runtime_error("SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2500 - " + _("failed to read alias from alias DB"));

	if(!IsSyscoinTxMine(aliastx, "alias")) {
		throw runtime_error("SYSCOIN_CERTIFICATE_CONSENSUS_ERROR ERRCODE: 2501 - " + _("This alias is not yours"));
	}
	wtxAliasIn = pwalletMain->GetWalletTx(aliastx.GetHash());
	if (wtxAliasIn == NULL)
		throw runtime_error("SYSCOIN_CERTIFICATE_CONSENSUS_ERROR ERRCODE: 2502 - " + _("This alias is not in your wallet"));


	if(params.size() >= 6)
		vchCat = vchFromValue(params[5]);
	bool bPrivate = false;

	if(params.size() >= 4)
	{
		bPrivate = boost::lexical_cast<int>(params[3].get_str()) == 1? true: false;
	}
	string strSafeSearch = "Yes";
	if(params.size() >= 5)
	{
		strSafeSearch = params[4].get_str();
	}
    if (vchData.size() < 1)
	{
        vchData = vchFromString(" ");
		bPrivate = false;
	}
    // gather inputs
	vector<unsigned char> vchCert = vchFromString(GenerateSyscoinGuid());
    // this is a syscoin transaction
    CWalletTx wtx;

	EnsureWalletIsUnlocked();
    CScript scriptPubKeyOrig;
	CPubKey aliasKey(theAlias.vchPubKey);
	scriptPubKeyOrig = GetScriptForDestination(aliasKey.GetID());

    CScript scriptPubKey;

    

	if(bPrivate)
	{
		string strCipherText;
		if(!EncryptMessage(theAlias.vchPubKey, vchData, strCipherText))
		{
			throw runtime_error("SYSCOIN_CERTIFICATE_RPC_ERROR: ERRCODE: 2503 - " + _("Could not encrypt certificate data"));
		}
		vchData = vchFromString(strCipherText);
	}

	// calculate net
    // build cert object
    CCert newCert;
	newCert.vchCert = vchCert;
	newCert.sCategory = vchCat;
    newCert.vchTitle = vchTitle;
	newCert.vchData = vchData;
	newCert.vchViewAlias = vchAlias;
	newCert.nHeight = chainActive.Tip()->nHeight;
	newCert.vchAlias = vchAlias;
	newCert.bPrivate = bPrivate;
	newCert.safetyLevel = 0;
	newCert.safeSearch = strSafeSearch == "Yes"? true: false;


	const vector<unsigned char> &data = newCert.Serialize();
    uint256 hash = Hash(data.begin(), data.end());
 	vector<unsigned char> vchHash = CScriptNum(hash.GetCheapHash()).getvch();
    vector<unsigned char> vchHashCert = vchFromValue(HexStr(vchHash));

    scriptPubKey << CScript::EncodeOP_N(OP_CERT_ACTIVATE) << vchCert << vchHashCert << OP_2DROP << OP_DROP;
    scriptPubKey += scriptPubKeyOrig;
	CScript scriptPubKeyAlias;
	if(theAlias.multiSigInfo.vchAliases.size() > 0)
		scriptPubKeyOrig = CScript(theAlias.multiSigInfo.vchRedeemScript.begin(), theAlias.multiSigInfo.vchRedeemScript.end());
	scriptPubKeyAlias << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << theAlias.vchAlias << theAlias.vchGUID << vchFromString("") << OP_2DROP << OP_2DROP;
	scriptPubKeyAlias += scriptPubKeyOrig;

	// use the script pub key to create the vecsend which sendmoney takes and puts it into vout
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

	
	
	
	SendMoneySyscoin(vecSend, recipient.nAmount+fee.nAmount+aliasRecipient.nAmount, false, wtx, wtxAliasIn, theAlias.multiSigInfo.vchAliases.size() > 0);
	UniValue res(UniValue::VARR);
	if(theAlias.multiSigInfo.vchAliases.size() > 0)
	{
		UniValue signParams(UniValue::VARR);
		signParams.push_back(EncodeHexTx(wtx));
		const UniValue &resSign = tableRPC.execute("syscoinsignrawtransaction", signParams);
		const UniValue& so = resSign.get_obj();
		string hex_str = "";

		const UniValue& hex_value = find_value(so, "hex");
		if (hex_value.isStr())
			hex_str = hex_value.get_str();
		const UniValue& complete_value = find_value(so, "complete");
		bool bComplete = false;
		if (complete_value.isBool())
			bComplete = complete_value.get_bool();
		if(bComplete)
		{
			res.push_back(wtx.GetHash().GetHex());
			res.push_back(stringFromVch(vchCert));
		}
		else
		{
			res.push_back(hex_str);
			res.push_back(stringFromVch(vchCert));
			res.push_back("false");
		}
	}
	else
	{
		res.push_back(wtx.GetHash().GetHex());
		res.push_back(stringFromVch(vchCert));
	}
	return res;
}

UniValue certupdate(const UniValue& params, bool fHelp) {
    if (fHelp || params.size() < 5 || params.size() > 8)
        throw runtime_error(
		"certupdate <guid> <alias> <title> <data> <private> [safesearch=Yes] [category=certificates] [viewalias='']\n"
                        "Perform an update on an certificate you control.\n"
                        "<guid> certificate guidkey.\n"
						"<alias> an alias you own to associate with this certificate.\n"
                        "<title> certificate title, 255 bytes max.\n"
                        "<data> certificate data, 1KB max.\n"
						"<private> set to 1 if you only want to make the cert data private, only the owner of the cert can view it.\n"
						"<category> category, 255 chars max. Defaults to certificates\n"
						"<viewalias> Allow this alias to view certificate private data.\n"
                        + HelpRequiringPassphrase());
    // gather & validate inputs
    vector<unsigned char> vchCert = vchFromValue(params[0]);
	vector<unsigned char> vchAlias = vchFromValue(params[1]);
    vector<unsigned char> vchTitle = vchFromValue(params[2]);
    vector<unsigned char> vchData = vchFromValue(params[3]);
	vector<unsigned char> vchCat = vchFromString("certificates");
	vector<unsigned char> vchViewData;
	vector<unsigned char> vchViewAlias;
	if(params.size() >= 8)
		vchViewAlias = vchFromValue(params[7]);

	if(params.size() >= 7)
		vchCat = vchFromValue(params[6]);
	bool bPrivate = boost::lexical_cast<int>(params[4].get_str()) == 1? true: false;
	string strSafeSearch = "Yes";
	if(params.size() >= 6)
	{
		strSafeSearch = params[5].get_str();
	}

    if (vchData.size() < 1)
        vchData = vchFromString(" ");
    // this is a syscoind txn
    CWalletTx wtx;
    CScript scriptPubKeyOrig;

    EnsureWalletIsUnlocked();

    // look for a transaction with this key
    CTransaction tx;
	CCert theCert;
	
    if (!GetTxOfCert( vchCert, theCert, tx, true))
        throw runtime_error("SYSCOIN_CERTIFICATE_RPC_ERROR: ERRCODE: 2504 - " + _("Could not find a certificate with this key"));

	CTransaction aliastx, viewaliastx;
	CAliasIndex theAlias, viewAlias;
	const CWalletTx *wtxAliasIn = NULL;
	if (!GetTxOfAlias(theCert.vchAlias, theAlias, aliastx, true))
		throw runtime_error("SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2505 - " + _("Failed to read alias from alias DB"));
	if(!IsSyscoinTxMine(aliastx, "alias")) {
		throw runtime_error("SYSCOIN_CERTIFICATE_CONSENSUS_ERROR ERRCODE: 2506 - " + _("This alias is not yours"));
	}
	wtxAliasIn = pwalletMain->GetWalletTx(aliastx.GetHash());
	if (wtxAliasIn == NULL)
		throw runtime_error("SYSCOIN_CERTIFICATE_CONSENSUS_ERROR ERRCODE: 2507 - " + _("This alias is not in your wallet"));

	if(!GetTxOfAlias(vchViewAlias, viewAlias, viewaliastx, true))
		vchViewAlias.clear();
			
	CCert copyCert = theCert;
	theCert.ClearCert();
	CPubKey currentKey(theAlias.vchPubKey);
	scriptPubKeyOrig = GetScriptForDestination(currentKey.GetID());

    // create CERTUPDATE txn keys
    CScript scriptPubKey;
	// if we want to make data private, encrypt it
	if(bPrivate)
	{
		vector<unsigned char> vchPubKeyPrivate = theAlias.vchPubKey;
		if(!vchAlias.empty())
		{
			CTransaction aliastmptx;
			CAliasIndex privateAlias;
			if (!GetTxOfAlias(vchAlias, privateAlias, aliastmptx, true))
				throw runtime_error("SYSCOIN_CERTIFICATE_CONSENSUS_ERROR: ERRCODE: 2505 - " + _("Failed to read alias from alias DB"));
			vchPubKeyPrivate = privateAlias.vchPubKey;
		}
		string strCipherText;
		if(!EncryptMessage(vchPubKeyPrivate, vchData, strCipherText))
		{
			throw runtime_error("SYSCOIN_CERTIFICATE_RPC_ERROR: ERRCODE: 2509 - " + _("Could not encrypt certificate data"));
		}
		vector<unsigned char> vchPubKeyViewPrivate = theAlias.vchPubKey;
		string strCipherViewText = "";
		if(!viewAlias.IsNull())
		{
			vchPubKeyViewPrivate = viewAlias.vchPubKey;
			string strCipherViewText = "";
			if(!EncryptMessage(vchPubKeyViewPrivate, vchData, strCipherViewText))
			{
				throw runtime_error("SYSCOIN_CERTIFICATE_RPC_ERROR: ERRCODE: 2509 - " + _("Could not encrypt certificate data"));
			}
		}
		vchData = vchFromString(strCipherText);
		vchViewData = vchFromString(strCipherViewText);
	}

    if(copyCert.vchTitle != vchTitle)
		theCert.vchTitle = vchTitle;
	if(copyCert.vchData != vchData)
		theCert.vchData = vchData;
	theCert.vchViewData = vchViewData;
	theCert.vchViewAlias = vchViewAlias;
	if(copyCert.sCategory != vchCat)
		theCert.sCategory = vchCat;
	theCert.vchAlias = theAlias.vchAlias;
	if(!vchAlias.empty() && vchAlias != theAlias.vchAlias)
		theCert.vchLinkAlias = vchAlias;
	theCert.nHeight = chainActive.Tip()->nHeight;
	theCert.bPrivate = bPrivate;
	theCert.safeSearch = strSafeSearch == "Yes"? true: false;

	const vector<unsigned char> &data = theCert.Serialize();
    uint256 hash = Hash(data.begin(), data.end());
 	vector<unsigned char> vchHash = CScriptNum(hash.GetCheapHash()).getvch();
    vector<unsigned char> vchHashCert = vchFromValue(HexStr(vchHash));
    scriptPubKey << CScript::EncodeOP_N(OP_CERT_UPDATE) << vchCert << vchHashCert << OP_2DROP << OP_DROP;
    scriptPubKey += scriptPubKeyOrig;

	vector<CRecipient> vecSend;
	CRecipient recipient;
	CreateRecipient(scriptPubKey, recipient);
	vecSend.push_back(recipient);
	CScript scriptPubKeyAlias;
	if(theAlias.multiSigInfo.vchAliases.size() > 0)
		scriptPubKeyOrig = CScript(theAlias.multiSigInfo.vchRedeemScript.begin(), theAlias.multiSigInfo.vchRedeemScript.end());
	scriptPubKeyAlias << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << theAlias.vchAlias << theAlias.vchGUID << vchFromString("") << OP_2DROP << OP_2DROP;
	scriptPubKeyAlias += scriptPubKeyOrig;
	CRecipient aliasRecipient;
	CreateRecipient(scriptPubKeyAlias, aliasRecipient);
	vecSend.push_back(aliasRecipient);
	
	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);
	
	
	
	SendMoneySyscoin(vecSend, recipient.nAmount+aliasRecipient.nAmount+fee.nAmount, false, wtx, wtxAliasIn, theAlias.multiSigInfo.vchAliases.size() > 0);	
 	UniValue res(UniValue::VARR);
	if(theAlias.multiSigInfo.vchAliases.size() > 0)
	{
		UniValue signParams(UniValue::VARR);
		signParams.push_back(EncodeHexTx(wtx));
		const UniValue &resSign = tableRPC.execute("syscoinsignrawtransaction", signParams);
		const UniValue& so = resSign.get_obj();
		string hex_str = "";

		const UniValue& hex_value = find_value(so, "hex");
		if (hex_value.isStr())
			hex_str = hex_value.get_str();
		const UniValue& complete_value = find_value(so, "complete");
		bool bComplete = false;
		if (complete_value.isBool())
			bComplete = complete_value.get_bool();
		if(bComplete)
			res.push_back(wtx.GetHash().GetHex());
		else
		{
			res.push_back(hex_str);
			res.push_back("false");
		}
	}
	else
	{
		res.push_back(wtx.GetHash().GetHex());
	}
	return res;
}


UniValue certtransfer(const UniValue& params, bool fHelp) {
 if (fHelp || params.size() != 2)
        throw runtime_error(
		"certtransfer <certkey> <alias>\n"
                "<certkey> certificate guidkey.\n"
				"<alias> Alias to transfer this certificate to.\n"
                 + HelpRequiringPassphrase());

    // gather & validate inputs
	vector<unsigned char> vchCert = vchFromValue(params[0]);
	vector<unsigned char> vchAlias = vchFromValue(params[1]);

	// check for alias existence in DB
	CTransaction tx;
	CAliasIndex toAlias;
	if (!GetTxOfAlias(vchAlias, toAlias, tx, true))
		throw runtime_error("SYSCOIN_CERTIFICATE_RPC_ERROR: ERRCODE: 2510 - " + _("Failed to read transfer alias from DB"));

	CPubKey xferKey = CPubKey(toAlias.vchPubKey);


	CSyscoinAddress sendAddr;
    // this is a syscoin txn
    CWalletTx wtx;
    CScript scriptPubKeyOrig, scriptPubKeyFromOrig;

    EnsureWalletIsUnlocked();
    CTransaction aliastx;
	CCert theCert;
    if (!GetTxOfCert( vchCert, theCert, tx, true))
        throw runtime_error("SYSCOIN_CERTIFICATE_RPC_ERROR: ERRCODE: 2511 - " + _("Could not find a certificate with this key"));

	CAliasIndex fromAlias;
	const CWalletTx *wtxAliasIn = NULL;
	if(!GetTxOfAlias(theCert.vchAlias, fromAlias, aliastx, true))
	{
		 throw runtime_error("SYSCOIN_CERTIFICATE_RPC_ERROR: ERRCODE: 2512 - " + _("Could not find the certificate alias"));
	}
	if(!IsSyscoinTxMine(aliastx, "alias")) {
		throw runtime_error("SYSCOIN_CERTIFICATE_CONSENSUS_ERROR ERRCODE: 2513 - " + _("This alias is not yours"));
	}
	wtxAliasIn = pwalletMain->GetWalletTx(aliastx.GetHash());
	if (wtxAliasIn == NULL)
		throw runtime_error("SYSCOIN_CERTIFICATE_CONSENSUS_ERROR ERRCODE: 2514 - " + _("This alias is not in your wallet"));

	CPubKey fromKey = CPubKey(fromAlias.vchPubKey);

	// if cert is private, decrypt the data
	vector<unsigned char> vchData = theCert.vchData;
	if(theCert.bPrivate)
	{		
		string strData = "";
		string strDecryptedData = "";
		string strCipherText;
		
		// decrypt using old key
		if(DecryptMessage(fromAlias.vchPubKey, theCert.vchData, strData))
			strDecryptedData = strData;
		else
			throw runtime_error("SYSCOIN_CERTIFICATE_RPC_ERROR: ERRCODE: 2516 - " + _("Could not decrypt certificate data"));
		// encrypt using new key
		if(!EncryptMessage(toAlias.vchPubKey, vchFromString(strDecryptedData), strCipherText))
		{
			throw runtime_error("SYSCOIN_CERTIFICATE_RPC_ERROR: ERRCODE: 2517 - " + _("Could not encrypt certificate data"));
		}
		vchData = vchFromString(strCipherText);
	}	
	CCert copyCert = theCert;
	theCert.ClearCert();
    scriptPubKeyOrig= GetScriptForDestination(xferKey.GetID());
	scriptPubKeyFromOrig= GetScriptForDestination(fromKey.GetID());
	if(fromAlias.multiSigInfo.vchAliases.size() > 0)
		scriptPubKeyFromOrig = CScript(fromAlias.multiSigInfo.vchRedeemScript.begin(), fromAlias.multiSigInfo.vchRedeemScript.end());
    CScript scriptPubKey;
	theCert.nHeight = chainActive.Tip()->nHeight;
	theCert.vchAlias = fromAlias.vchAlias;
	theCert.vchLinkAlias = toAlias.vchAlias;
	theCert.bPrivate = copyCert.bPrivate;
	theCert.safeSearch = copyCert.safeSearch;
	theCert.safetyLevel = copyCert.safetyLevel;
	if(copyCert.vchData != vchData)
		theCert.vchData = vchData;

	const vector<unsigned char> &data = theCert.Serialize();
    uint256 hash = Hash(data.begin(), data.end());
 	vector<unsigned char> vchHash = CScriptNum(hash.GetCheapHash()).getvch();
    vector<unsigned char> vchHashCert = vchFromValue(HexStr(vchHash));
    scriptPubKey << CScript::EncodeOP_N(OP_CERT_TRANSFER) << vchCert << vchHashCert << OP_2DROP << OP_DROP;
	scriptPubKey += scriptPubKeyOrig;
    // send the cert pay txn
	vector<CRecipient> vecSend;
	CRecipient recipient;
	CreateRecipient(scriptPubKey, recipient);
	vecSend.push_back(recipient);

	CScript scriptPubKeyAlias;
	scriptPubKeyAlias << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << fromAlias.vchAlias << fromAlias.vchGUID << vchFromString("") << OP_2DROP << OP_2DROP;
	scriptPubKeyAlias += scriptPubKeyFromOrig;
	CRecipient aliasRecipient;
	CreateRecipient(scriptPubKeyAlias, aliasRecipient);
	vecSend.push_back(aliasRecipient);

	CScript scriptData;
	scriptData << OP_RETURN << data;
	CRecipient fee;
	CreateFeeRecipient(scriptData, data, fee);
	vecSend.push_back(fee);
	
	
	
	SendMoneySyscoin(vecSend, recipient.nAmount+aliasRecipient.nAmount+fee.nAmount, false, wtx, wtxAliasIn, fromAlias.multiSigInfo.vchAliases.size() > 0);

	UniValue res(UniValue::VARR);
	if(fromAlias.multiSigInfo.vchAliases.size() > 0)
	{
		UniValue signParams(UniValue::VARR);
		signParams.push_back(EncodeHexTx(wtx));
		const UniValue &resSign = tableRPC.execute("syscoinsignrawtransaction", signParams);
		const UniValue& so = resSign.get_obj();
		string hex_str = "";

		const UniValue& hex_value = find_value(so, "hex");
		if (hex_value.isStr())
			hex_str = hex_value.get_str();
		const UniValue& complete_value = find_value(so, "complete");
		bool bComplete = false;
		if (complete_value.isBool())
			bComplete = complete_value.get_bool();
		if(bComplete)
			res.push_back(wtx.GetHash().GetHex());
		else
		{
			res.push_back(hex_str);
			res.push_back("false");
		}
	}
	else
	{
		res.push_back(wtx.GetHash().GetHex());
	}
	return res;
}


UniValue certinfo(const UniValue& params, bool fHelp) {
    if (fHelp || 1 != params.size())
        throw runtime_error("certinfo <guid>\n"
                "Show stored values of a single certificate and its .\n");

    vector<unsigned char> vchCert = vchFromValue(params[0]);

    // look for a transaction with this key, also returns
    // an cert object if it is found
    CTransaction tx;

	vector<CCert> vtxPos;

	int expired = 0;
	int expires_in = 0;
	int expired_block = 0;
	UniValue oCert(UniValue::VOBJ);
    vector<unsigned char> vchValue;

	if (!pcertdb->ReadCert(vchCert, vtxPos) || vtxPos.empty())
		throw runtime_error("failed to read from cert DB");
	CCert ca = vtxPos.back();
	if(ca.safetyLevel >= SAFETY_LEVEL2)
		throw runtime_error("cert has been banned");
	if (!GetSyscoinTransaction(ca.nHeight, ca.txHash, tx, Params().GetConsensus()))
		throw runtime_error("failed to read transaction from disk");   


	// check that the seller isn't banned level 2
	CAliasIndex alias;
	CTransaction aliastx;
	if (!GetTxOfAlias(ca.vchAlias, alias, aliastx, true))
		throw runtime_error("failed to read xfer alias from alias DB");
	
	if(alias.safetyLevel >= SAFETY_LEVEL2)
		throw runtime_error("cert owner has been banned");

    string sHeight = strprintf("%llu", ca.nHeight);
    oCert.push_back(Pair("cert", stringFromVch(vchCert)));
    oCert.push_back(Pair("txid", ca.txHash.GetHex()));
    oCert.push_back(Pair("height", sHeight));
    oCert.push_back(Pair("title", stringFromVch(ca.vchTitle)));
	string strData = stringFromVch(ca.vchData);
	string strDecrypted = "";
	if(ca.bPrivate)
	{
		strData = _("Encrypted for owner of certificate private data");
		if(!ca.vchViewData.empty() && !ca.vchViewAlias.empty())	
		{
			CAliasIndex aliasView;
			CTransaction aliasviewtx;
			if (!GetTxOfAlias(ca.vchViewAlias, aliasView, aliasviewtx, true))
				throw runtime_error("failed to read xfer alias from alias DB");
			if(DecryptMessage(aliasView.vchPubKey, ca.vchViewData, strDecrypted))
				strData = strDecrypted;	
		}
		if(!ca.vchData.empty() && strDecrypted == "")
		{
			if(DecryptMessage(alias.vchPubKey, ca.vchData, strDecrypted))
				strData = strDecrypted;		
		}
	}
    oCert.push_back(Pair("data", strData));
	oCert.push_back(Pair("category", stringFromVch(ca.sCategory)));
	oCert.push_back(Pair("private", ca.bPrivate? "Yes": "No"));
	oCert.push_back(Pair("safesearch", ca.safeSearch ? "Yes" : "No"));
	oCert.push_back(Pair("safetylevel", ca.safetyLevel));
    oCert.push_back(Pair("ismine", IsSyscoinTxMine(aliastx, "alias") ? "true" : "false"));

    uint64_t nHeight;
	nHeight = ca.nHeight;
	oCert.push_back(Pair("alias", stringFromVch(ca.vchAlias)));
	oCert.push_back(Pair("viewalias", stringFromVch(ca.vchViewAlias)));
	expired_block = nHeight + GetCertExpirationDepth();
    if(expired_block < chainActive.Tip()->nHeight)
	{
		expired = 1;
	}  
	expires_in = expired_block - chainActive.Tip()->nHeight;
	oCert.push_back(Pair("expires_in", expires_in));
	oCert.push_back(Pair("expires_on", expired_block));
	oCert.push_back(Pair("expired", expired));
    return oCert;
}

UniValue certlist(const UniValue& params, bool fHelp) {
    if (fHelp || 2 < params.size() || params.size() < 1)
        throw runtime_error("certlist <alias> [<cert>]\n"
                "list certificates that an alias owns");
	vector<unsigned char> vchCert;
	vector<unsigned char> vchAlias = vchFromValue(params[0]);
	string name = stringFromVch(vchAlias);
	vector<CAliasIndex> vtxPos;
	if (!paliasdb->ReadAlias(vchAlias, vtxPos) || vtxPos.empty())
		throw runtime_error("failed to read from alias DB");
	const CAliasIndex &alias = vtxPos.back();
	CTransaction aliastx;
	uint256 txHash;
	if (!GetSyscoinTransaction(alias.nHeight, alias.txHash, aliastx, Params().GetConsensus()))
	{
		throw runtime_error("failed to read alias transaction");
	}
    vector<unsigned char> vchNameUniq;
    if (params.size() == 2)
        vchNameUniq = vchFromValue(params[1]);

    UniValue oRes(UniValue::VARR);
    map< vector<unsigned char>, int > vNamesI;
    map< vector<unsigned char>, UniValue > vNamesO;

    CTransaction tx;
    uint64_t nHeight;
	for(std::vector<CAliasIndex>::reverse_iterator it = vtxPos.rbegin(); it != vtxPos.rend(); ++it) {
		const CAliasIndex& theAlias = *it;
		if(!GetSyscoinTransaction(theAlias.nHeight, theAlias.txHash, tx, Params().GetConsensus()))
			continue;
		
		int expired = 0;
		int expires_in = 0;
		int expired_block = 0;  

		// decode txn, skip non-cert txns
		vector<vector<unsigned char> > vvch;
		int op, nOut;
		if (!DecodeCertTx(tx, op, nOut, vvch))
			continue;

		vchCert = vvch[0];
		
		
		// skip this cert if it doesn't match the given filter value
		if (vchNameUniq.size() > 0 && vchNameUniq != vchCert)
			continue;
			
		vector<CCert> vtxCertPos;
		if (!pcertdb->ReadCert(vchCert, vtxCertPos) || vtxCertPos.empty())
			continue;
		const CCert &cert = vtxCertPos.back();
		if(cert.vchAlias != vchAlias)
			continue;		
		nHeight = cert.nHeight;
		// get last active name only
		if (vNamesI.find(vchCert) != vNamesI.end() && (nHeight <= vNamesI[vchCert] || vNamesI[vchCert] < 0))
			continue;

		
        // build the output object
		UniValue oName(UniValue::VOBJ);
        oName.push_back(Pair("cert", stringFromVch(vchCert)));
        oName.push_back(Pair("title", stringFromVch(cert.vchTitle)));
		
		string strData = stringFromVch(cert.vchData);
		bool isExpired = false;
		string strDecrypted = "";
		if(cert.bPrivate)
		{
			strData = _("Encrypted for owner of certificate private data");
			if(!cert.vchViewData.empty() && !cert.vchViewAlias.empty())	
			{
				CAliasIndex aliasView;
				CTransaction aliasviewtx;
				if (!GetTxOfAlias(cert.vchViewAlias, aliasView, aliasviewtx, true))
					throw runtime_error("failed to read xfer alias from alias DB");
				if(DecryptMessage(aliasView.vchPubKey, cert.vchViewData, strDecrypted))
					strData = strDecrypted;	
			}
			if(!cert.vchData.empty() && strDecrypted == "")
			{
				if(DecryptMessage(theAlias.vchPubKey, cert.vchData, strDecrypted))
					strData = strDecrypted;		
			}
		}
		oName.push_back(Pair("private", cert.bPrivate? "Yes": "No"));
		oName.push_back(Pair("safesearch", cert.safeSearch ? "Yes" : "No"));
		oName.push_back(Pair("safetylevel", cert.safetyLevel));
		oName.push_back(Pair("data", strData));
		oName.push_back(Pair("category", stringFromVch(cert.sCategory)));
		oName.push_back(Pair("alias", stringFromVch(cert.vchAlias)));
		oName.push_back(Pair("viewalias", stringFromVch(cert.vchViewAlias)));
		oName.push_back(Pair("ismine", IsSyscoinTxMine(aliastx, "alias") ? "true" : "false"));
		expired_block = nHeight + GetCertExpirationDepth();
        if(expired_block < chainActive.Tip()->nHeight)
		{
			expired = 1;
		}  
		expires_in = expired_block - chainActive.Tip()->nHeight;
		oName.push_back(Pair("expires_in", expires_in));
		oName.push_back(Pair("expires_on", expired_block));
		oName.push_back(Pair("expired", expired));
 
		vNamesI[vchCert] = nHeight;
		vNamesO[vchCert] = oName;	
    
	}
    BOOST_FOREACH(const PAIRTYPE(vector<unsigned char>, UniValue)& item, vNamesO)
        oRes.push_back(item.second);
    return oRes;
}


UniValue certhistory(const UniValue& params, bool fHelp) {
    if (fHelp || 1 != params.size())
        throw runtime_error("certhistory <cert>\n"
                "List all stored values of an cert.\n");

    UniValue oRes(UniValue::VARR);
    vector<unsigned char> vchCert = vchFromValue(params[0]);
	
    string cert = stringFromVch(vchCert);

    {
        vector<CCert> vtxPos;
        if (!pcertdb->ReadCert(vchCert, vtxPos) || vtxPos.empty())
            throw runtime_error("failed to read from cert DB");

        CCert txPos2;
        uint256 txHash;
        BOOST_FOREACH(txPos2, vtxPos) {
			CTransaction tx;
            txHash = txPos2.txHash;
			if (!GetSyscoinTransaction(txPos2.nHeight, txHash, tx, Params().GetConsensus())) {
				error("could not read txpos");
				continue;
			}
            // decode txn, skip non-alias txns
            vector<vector<unsigned char> > vvch;
            int op, nOut;
            if (!DecodeCertTx(tx, op, nOut, vvch) 
            	|| !IsCertOp(op) )
                continue;
			int expired = 0;
			int expires_in = 0;
			int expired_block = 0;
            UniValue oCert(UniValue::VOBJ);
            vector<unsigned char> vchValue;
            uint64_t nHeight;
			nHeight = txPos2.nHeight;
            oCert.push_back(Pair("cert", cert));
			string opName = certFromOp(op);
			oCert.push_back(Pair("certtype", opName));
			string strDecrypted = "";
			string strData = "";
			CTransaction aliastx;
			CAliasIndex theAlias;
			bool isExpired = false;
			vector<CAliasIndex> aliasVtxPos;
			if(GetTxAndVtxOfAlias(txPos2.vchAlias, theAlias, aliastx, aliasVtxPos, isExpired, true))
			{
				theAlias.nHeight = txPos2.nHeight;
				theAlias.GetAliasFromList(aliasVtxPos);
			}
			if(txPos2.bPrivate)
			{
				strData = _("Encrypted for owner of certificate private data");
				if(!txPos2.vchViewData.empty() && !txPos2.vchViewAlias.empty())	
				{
					CAliasIndex aliasView;
					CTransaction aliasviewtx;
					if (!GetTxOfAlias(txPos2.vchViewAlias, aliasView, aliasviewtx, true))
						throw runtime_error("failed to read xfer alias from alias DB");
					if(DecryptMessage(aliasView.vchPubKey, txPos2.vchViewData, strDecrypted))
						strData = strDecrypted;	
				}
				if(!txPos2.vchData.empty() && strDecrypted == "")
				{
					if(DecryptMessage(theAlias.vchPubKey, txPos2.vchData, strDecrypted))
						strData = strDecrypted;		
				}
			}
			oCert.push_back(Pair("private", txPos2.bPrivate? "Yes": "No"));
			oCert.push_back(Pair("data", strData));
			oCert.push_back(Pair("category", stringFromVch(txPos2.sCategory)));
            oCert.push_back(Pair("txid", tx.GetHash().GetHex()));
			oCert.push_back(Pair("alias", stringFromVch(txPos2.vchAlias)));
			oCert.push_back(Pair("viewalias", stringFromVch(txPos2.vchViewAlias)));
			expired_block = nHeight + GetCertExpirationDepth();
            if(expired_block < chainActive.Tip()->nHeight)
			{
				expired = 1;
			}  
			expires_in = expired_block - chainActive.Tip()->nHeight;
			oCert.push_back(Pair("expires_in", expires_in));
			oCert.push_back(Pair("expires_on", expired_block));
			oCert.push_back(Pair("expired", expired));
            oRes.push_back(oCert);
        }
    }
    return oRes;
}
UniValue certfilter(const UniValue& params, bool fHelp) {
	if (fHelp || params.size() > 4)
		throw runtime_error(
				"certfilter [[[[[regexp]] from=0]] safesearch='Yes' category]\n"
						"scan and filter certs\n"
						"[regexp] : apply [regexp] on certs, empty means all certs\n"
						"[from] : show results from this GUID [from], 0 means first.\n"
						"[certfilter] : shows all certs that are safe to display (not on the ban list)\n"
						"[safesearch] : shows all certs that are safe to display (not on the ban list)\n"
						"[category] : category you want to search in, empty for all\n"
						"certfilter \"\" 5 # list certs updated in last 5 blocks\n"
						"certfilter \"^cert\" # list all certs starting with \"cert\"\n"
						"certfilter 36000 0 0 stat # display stats (number of certs) on active certs\n");

	vector<unsigned char> vchCert;
	string strRegexp;
	string strCategory;
	bool safeSearch = true;


	if (params.size() > 0)
		strRegexp = params[0].get_str();

	if (params.size() > 1)
		vchCert = vchFromValue(params[1]);

	if (params.size() > 2)
		safeSearch = params[2].get_str()=="On"? true: false;

	if (params.size() > 3)
		strCategory = params[3].get_str();

    UniValue oRes(UniValue::VARR);
    
    vector<pair<vector<unsigned char>, CCert> > certScan;
    if (!pcertdb->ScanCerts(vchCert, strRegexp, safeSearch, strCategory, 25, certScan))
        throw runtime_error("scan failed");
    pair<vector<unsigned char>, CCert> pairScan;
	BOOST_FOREACH(pairScan, certScan) {
		const CCert &txCert = pairScan.second;
		const string &cert = stringFromVch(pairScan.first);

       int nHeight = txCert.nHeight;

		int expired = 0;
		int expires_in = 0;
		int expired_block = 0;
        UniValue oCert(UniValue::VOBJ);
        oCert.push_back(Pair("cert", cert));
		vector<unsigned char> vchValue = txCert.vchTitle;
        string value = stringFromVch(vchValue);
        oCert.push_back(Pair("title", value));

		string strData = stringFromVch(txCert.vchData);
		string strDecrypted = "";
		CTransaction aliastx;
		CAliasIndex theAlias;
		bool isExpired = false;
		vector<CAliasIndex> aliasVtxPos;
		if(GetTxAndVtxOfAlias(txCert.vchAlias, theAlias, aliastx, aliasVtxPos, isExpired, true))
		{
			theAlias.nHeight = txCert.nHeight;
			theAlias.GetAliasFromList(aliasVtxPos);
		}
		if(txCert.bPrivate)
		{
			strData = _("Encrypted for owner of certificate private data");
			if(!txCert.vchViewData.empty() && !txCert.vchViewAlias.empty())	
			{
				CAliasIndex aliasView;
				CTransaction aliasviewtx;
				if (!GetTxOfAlias(txCert.vchViewAlias, aliasView, aliasviewtx, true))
					throw runtime_error("failed to read xfer alias from alias DB");
				if(DecryptMessage(aliasView.vchPubKey, txCert.vchViewData, strDecrypted))
					strData = strDecrypted;	
			}
			if(!txCert.vchData.empty() && strDecrypted == "")
			{
				if(DecryptMessage(theAlias.vchPubKey, txCert.vchData, strDecrypted))
					strData = strDecrypted;		
			}
		}

		oCert.push_back(Pair("data", strData));
		oCert.push_back(Pair("category", stringFromVch(txCert.sCategory)));
		oCert.push_back(Pair("private", txCert.bPrivate? "Yes": "No"));
		expired_block = nHeight + GetCertExpirationDepth();
        if(expired_block < chainActive.Tip()->nHeight)
		{
			expired = 1;
		}  
		expires_in = expired_block - chainActive.Tip()->nHeight;
		oCert.push_back(Pair("expires_in", expires_in));
		oCert.push_back(Pair("expires_on", expired_block));
		oCert.push_back(Pair("expired", expired));
		oCert.push_back(Pair("alias", stringFromVch(txCert.vchAlias)));
		oCert.push_back(Pair("viewalias", stringFromVch(txCert.vchViewAlias)));
        oRes.push_back(oCert);
	}


	return oRes;
}
void CertTxToJSON(const int op, const std::vector<unsigned char> &vchData, const std::vector<unsigned char> &vchHash, UniValue &entry)
{
	string opName = certFromOp(op);
	CCert cert;
	if(!cert.UnserializeFromData(vchData, vchHash))
		throw runtime_error("SYSCOIN_CERTIFICATE_RPC_ERROR: ERRCODE: 2518 - " + _("Could not decoding syscoin transaction"));

	bool isExpired = false;
	vector<CAliasIndex> aliasVtxPos;
	vector<CCert> certVtxPos;
	CTransaction certtx, aliastx;
	CCert dbCert;
	if(GetTxAndVtxOfCert(cert.vchCert, dbCert, certtx, certVtxPos, true))
	{
		dbCert.nHeight = cert.nHeight;
		dbCert.GetCertFromList(certVtxPos);
	}
	CAliasIndex dbAlias;
	if(GetTxAndVtxOfAlias(cert.vchAlias, dbAlias, aliastx, aliasVtxPos, isExpired, true))
	{
		dbAlias.nHeight = cert.nHeight;
		dbAlias.GetAliasFromList(aliasVtxPos);
	}
	string noDifferentStr = _("<No Difference Detected>");

	entry.push_back(Pair("txtype", opName));
	entry.push_back(Pair("cert", stringFromVch(cert.vchCert)));

	string titleValue = noDifferentStr;
	if(!cert.vchTitle.empty() && cert.vchTitle != dbCert.vchTitle)
		titleValue = stringFromVch(cert.vchTitle);
	entry.push_back(Pair("title", titleValue));

	string strDataValue = "";
	if(cert.bPrivate)
	{
		if(!cert.vchData.empty())
			strDataValue = _("Encrypted for owner of certificate private data");
		string strDecrypted = "";
		if(DecryptMessage(dbAlias.vchPubKey, cert.vchData, strDecrypted))
			strDataValue = strDecrypted;		
	}
	string dataValue = noDifferentStr;
	if(!cert.vchData.empty() && cert.vchData != dbCert.vchData)
		dataValue = strDataValue;

	entry.push_back(Pair("data", dataValue));


	string aliasValue = noDifferentStr;
	if(!cert.vchLinkAlias.empty() && cert.vchLinkAlias != dbCert.vchAlias)
		aliasValue = stringFromVch(cert.vchLinkAlias);
	if(cert.vchAlias != dbCert.vchAlias)
		aliasValue = stringFromVch(cert.vchAlias);

	entry.push_back(Pair("alias", aliasValue));

	string aliasViewValue = noDifferentStr;
	if(!cert.vchViewAlias.empty() && cert.vchViewAlias != dbCert.vchViewAlias)
		aliasViewValue = stringFromVch(cert.vchViewAlias);

	entry.push_back(Pair("viewalias", aliasViewValue));

	string categoryValue = noDifferentStr;
	if(!cert.sCategory.empty() && cert.sCategory != dbCert.sCategory)
		categoryValue = stringFromVch(cert.sCategory);

	entry.push_back(Pair("category", categoryValue ));


	string safeSearchValue = noDifferentStr;
	if(cert.safeSearch != dbCert.safeSearch)
		safeSearchValue = cert.safeSearch? "Yes": "No";

	entry.push_back(Pair("safesearch", safeSearchValue));

	string safetyLevelValue = noDifferentStr;
	if(cert.safetyLevel != dbCert.safetyLevel)
		safetyLevelValue = cert.safetyLevel;

	entry.push_back(Pair("safetylevel", safetyLevelValue));

	string privateValue = noDifferentStr;
	if(cert.bPrivate != dbCert.bPrivate)
		privateValue = cert.bPrivate? "Yes": "No";

	entry.push_back(Pair("private", privateValue ));


}



