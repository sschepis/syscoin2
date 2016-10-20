#ifndef CERT_H
#define CERT_H

#include "rpc/server.h"
#include "dbwrapper.h"
#include "script/script.h"
#include "serialize.h"
class CWalletTx;
class CTransaction;
class CReserveKey;
class CCoinsViewCache;
class CCoins;
class CBlock;
bool CheckCertInputs(const CTransaction &tx, int op, int nOut, const std::vector<std::vector<unsigned char> > &vvchArgs, const CCoinsViewCache &inputs, bool fJustCheck, int nHeight, std::string &errorMessage, const CBlock* block = NULL, bool dontaddtodb=false);
bool DecodeCertTx(const CTransaction& tx, int& op, int& nOut, std::vector<std::vector<unsigned char> >& vvch);
bool DecodeAndParseCertTx(const CTransaction& tx, int& op, int& nOut, std::vector<std::vector<unsigned char> >& vvch);
bool DecodeCertScript(const CScript& script, int& op, std::vector<std::vector<unsigned char> > &vvch);
bool IsCertOp(int op);
int IndexOfCertOutput(const CTransaction& tx);
bool EncryptMessage(const std::vector<unsigned char> &vchPublicKey, const std::vector<unsigned char> &vchMessage, std::string &strCipherText);
bool DecryptMessage(const std::vector<unsigned char> &vchPublicKey, const std::vector<unsigned char> &vchCipherText, std::string &strMessage);
void CertTxToJSON(const int op, const std::vector<unsigned char> &vchData, const std::vector<unsigned char> &vchHash, UniValue &entry);
std::string certFromOp(int op);
int GetCertExpirationDepth();
CScript RemoveCertScriptPrefix(const CScript& scriptIn);
extern bool IsSys21Fork(const uint64_t& nHeight);
class CCert {
public:
	std::vector<unsigned char> vchCert;
	std::vector<unsigned char> vchAlias;
	// to modify vchAlias in certtransfer
	std::vector<unsigned char> vchLinkAlias;
	std::vector<unsigned char> vchViewAlias;
    std::vector<unsigned char> vchTitle;
    std::vector<unsigned char> vchData;
	std::vector<unsigned char> vchViewData;
	std::vector<unsigned char> sCategory;
    uint256 txHash;
    uint64_t nHeight;
	bool bPrivate;
	unsigned char safetyLevel;
	bool safeSearch;
    CCert() {
        SetNull();
    }
    CCert(const CTransaction &tx) {
        SetNull();
        UnserializeFromTx(tx);
    }
	void ClearCert()
	{
		vchViewAlias.clear();
		vchLinkAlias.clear();
		vchData.clear();
		vchViewData.clear();
		vchTitle.clear();
		sCategory.clear();
	}
	ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
	inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
		READWRITE(vchTitle);		
		READWRITE(vchData);
		READWRITE(vchViewData);
		READWRITE(txHash);
		READWRITE(VARINT(nHeight));
		READWRITE(vchLinkAlias);
		READWRITE(vchViewAlias);
		READWRITE(bPrivate);
		READWRITE(vchCert);
		READWRITE(safetyLevel);
		READWRITE(safeSearch);
		READWRITE(sCategory);
		READWRITE(vchAlias);
	}
    friend bool operator==(const CCert &a, const CCert &b) {
        return (
        a.vchTitle == b.vchTitle
        && a.vchData == b.vchData
		&& a.vchViewData == b.vchViewData
        && a.txHash == b.txHash
        && a.nHeight == b.nHeight
		&& a.vchAlias == b.vchAlias
		&& a.vchLinkAlias == b.vchLinkAlias
		&& a.vchViewAlias == b.vchViewAlias
		&& a.bPrivate == b.bPrivate
		&& a.safetyLevel == b.safetyLevel
		&& a.safeSearch == b.safeSearch
		&& a.vchCert == b.vchCert
		&& a.sCategory == b.sCategory
        );
    }

    CCert operator=(const CCert &b) {
        vchTitle = b.vchTitle;
        vchData = b.vchData;
		vchViewData = b.vchViewData;
        txHash = b.txHash;
        nHeight = b.nHeight;
		vchAlias = b.vchAlias;
		vchLinkAlias = b.vchLinkAlias;
		vchViewAlias = b.vchViewAlias;
		bPrivate = b.bPrivate;
		safetyLevel = b.safetyLevel;
		safeSearch = b.safeSearch;
		vchCert = b.vchCert;
		sCategory = b.sCategory;
        return *this;
    }

    friend bool operator!=(const CCert &a, const CCert &b) {
        return !(a == b);
    }
    bool GetCertFromList(std::vector<CCert> &certList) {
        if(certList.size() == 0) return false;
		CCert myCert = certList.front();
		if(nHeight <= 0)
		{
			*this = myCert;
			return true;
		}
			
		// find the closest cert without going over in height, assuming certList orders entries by nHeight ascending
        for(std::vector<CCert>::reverse_iterator it = certList.rbegin(); it != certList.rend(); ++it) {
            const CCert &c = *it;
			// skip if this height is greater than our cert height
			if(c.nHeight > nHeight)
				continue;
            myCert = c;
			break;
        }
        *this = myCert;
        return true;
    }
    void SetNull() { vchViewAlias.clear(); vchLinkAlias.clear(); sCategory.clear(); vchCert.clear(); safetyLevel = 0; safeSearch = true; nHeight = 0; txHash.SetNull(); vchAlias.clear(); bPrivate = false; vchTitle.clear(); vchData.clear(); vchViewData.clear();}
    bool IsNull() const { return (vchViewAlias.empty() && vchLinkAlias.empty() && sCategory.empty() && vchCert.empty() && safetyLevel == 0 && safeSearch && !bPrivate && txHash.IsNull() &&  nHeight == 0 && vchData.empty() && vchViewData.empty() && vchTitle.empty() && vchAlias.empty()); }
    bool UnserializeFromTx(const CTransaction &tx);
	bool UnserializeFromData(const std::vector<unsigned char> &vchData, const std::vector<unsigned char> &vchHash);
	const std::vector<unsigned char> Serialize();
};


class CCertDB : public CDBWrapper {
public:
    CCertDB(size_t nCacheSize, bool fMemory, bool fWipe) : CDBWrapper(GetDataDir() / "certificates", nCacheSize, fMemory, fWipe) {}

    bool WriteCert(const std::vector<unsigned char>& name, std::vector<CCert>& vtxPos) {
        return Write(make_pair(std::string("certi"), name), vtxPos);
    }

    bool EraseCert(const std::vector<unsigned char>& name) {
        return Erase(make_pair(std::string("certi"), name));
    }

    bool ReadCert(const std::vector<unsigned char>& name, std::vector<CCert>& vtxPos) {
        return Read(make_pair(std::string("certi"), name), vtxPos);
    }

    bool ExistsCert(const std::vector<unsigned char>& name) {
        return Exists(make_pair(std::string("certi"), name));
    }

    bool ScanCerts(
		const std::vector<unsigned char>& vchCert, const std::string &strRegExp,  bool safeSearch, const std::string& strCategory,
            unsigned int nMax,
            std::vector<std::pair<std::vector<unsigned char>, CCert> >& certScan);

};
bool GetTxOfCert(const std::vector<unsigned char> &vchCert,
        CCert& txPos, CTransaction& tx, bool skipExpiresCheck=false);
bool GetTxAndVtxOfCert(const std::vector<unsigned char> &vchCert,
					   CCert& txPos, CTransaction& tx, std::vector<CCert> &vtxPos, bool skipExpiresCheck=false);
void PutToCertList(std::vector<CCert> &certList, CCert& index);
#endif // CERT_H
