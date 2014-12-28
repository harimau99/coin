/*######     Copyright (c) 1997-2015 Ufasoft  http://ufasoft.com  mailto:support@ufasoft.com,  Sergey Pavlov  mailto:dev@ufasoft.com #########################################################################################################
#                                                                                                                                                                                                                                            #
# This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation;  either version 3, or (at your option) any later version.          #
# This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.   #
# You should have received a copy of the GNU General Public License along with this program; If not, see <http://www.gnu.org/licenses/>                                                                                                      #
############################################################################################################################################################################################################################################*/

#include <el/ext.h>

#include "block-template.h"
#include "prime-util.h"


namespace Coin {

BinaryWriter& operator<<(BinaryWriter& wr, const MinerBlock& block) {
	block.WriteHeader(wr);

	CoinSerialized::WriteVarInt(wr, block.Txes.size());
	EXT_FOR (const MinerTx& mtx, block.Txes) {
		wr.BaseStream.WriteBuf(mtx.Data);
	}
	return wr;
}

void MinerBlock::ReadJson(const VarValue& json) {
	Ver = (uint32_t)json["version"].ToInt64();
	Height = uint32_t(json["height"].ToInt64());
	PrevBlockHash = HashValue(json["previousblockhash"].ToString());

	DifficultyTargetBits = Convert::ToUInt32(json["bits"].ToString(), 16);
	HashTarget = HashValue::FromDifficultyBits(DifficultyTargetBits);
	
	VarValue txes = json["transactions"];
	Txes.resize(txes.size()+1);

	SetTimestamps(DateTime::from_time_t(json["curtime"].ToInt64()));
#ifdef X_DEBUG//!!!D
	TRC(1, "Time_t: " << json["curtime"].ToInt64());
#endif

	if (json.HasKey("mintime"))
		MinTime = DateTime::from_time_t(json["mintime"].ToInt64());	

	if (json.HasKey("sizelimit"))
		SizeLimit = uint32_t(json["sizelimit"].ToInt64());
	if (json.HasKey("sigoplimit"))
		SigopLimit = uint32_t(json["sigoplimit"].ToInt64());

	NonceRange = FromOptionalNonceRange(json);

	MyExpireTime = DateTime::UtcNow() + TimeSpan::FromSeconds(json.HasKey("expires") ? int32_t(json["expires"].ToInt64()) : 600);

	if (VarValue coinbaseAux = json["coinbaseaux"]) {
		MemoryStream ms;
		vector<String> keys = coinbaseAux.Keys();
		EXT_FOR(RCString key, keys) {
			ms.WriteBuf(Blob::FromHexString(coinbaseAux[key].ToString()));
		}
		CoinbaseAux = ConstBuf(ms);
	}

	/*
	byte arPfx[] = { 1, 0, 0, 0, 1,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255, 255, 255,
		9, 0x03, byte(Height), byte(Height >> 8), byte(Height >> 16), 4 };
	Coinb1 = Blob(arPfx, sizeof arPfx);
	ExtraNonce2 = Blob(0, 4);		
	*/

	for (int i=0; i<txes.size(); ++i) {
		MinerTx& mtx = Txes[i+1];
		VarValue jtx = txes[i];
		mtx.Data = Blob::FromHexString(jtx["data"].ToString());
		if (VarValue vHash = jtx["hash"]) {
			mtx.Hash = HashValue(vHash.ToString());
#ifdef _DEBUG
			if (Hash(mtx.Data) != mtx.Hash)
				Throw(E_FAIL);
#endif
		} else
			mtx.Hash = Hash(mtx.Data);
	}

	struct TxHasher {
		const Coin::MinerBlock *minerBlock;

		HashValue operator()(const MinerTx& mtx, int n) const {
			return &mtx == &minerBlock->Txes[0] ? Hash(mtx.Data) : mtx.Hash;
		}
	} txHasher =  { this };
	MerkleBranch = BuildMerkleTree<Coin::HashValue>(Txes, txHasher, &HashValue::Combine).GetBranch(0);

	if (json.HasKey("workid"))
		WorkId = json["workid"].ToString();

	if (VarValue vv = json["coinbasevalue"])
		CoinbaseValue = vv.ToInt64();

	if (VarValue jtx = json["coinbasetxn"]) {
		Blob blob = Blob::FromHexString(jtx["data"].ToString());
		CoinbaseTxn = blob;
		if (blob.Size <= 36+4+1)
			Throw(E_FAIL);
		byte& lenScript = *(blob.data()+36+4+1);
		lenScript += 4;
		size_t cbFirst = 36+4+1+1+lenScript-4;
		
		Coinb1 = Blob(blob.data(), cbFirst);
//		Extranonce1 = Blob(0, 0);
		Coinb2 = Blob(blob.data()+cbFirst, blob.Size-cbFirst);
	}	
}

HashValue MinerBlock::MerkleRoot(bool bSave) const {
	if (!m_merkleRoot) {
		if (Txes.empty())
			Txes.resize(1);
		Txes[0].Data = Coinb1 + ExtraNonce1 + ExtraNonce2 + Coinb2;
		SHA256 sha;
		hashval hv = sha.ComputeHash(Txes[0].Data);
		if (Algo != HashAlgo::Sha3)
			hv = sha.ComputeHash(hv);
		m_merkleRoot = MerkleBranch.Apply(HashValue(hv));
#ifdef X_DEBUG//!!!D
		cout << "\nTx0:" << Txes[0].Data << "\n";
		cout << "\nTx0 Hash:" << Hash(Txes[0].Data) << "\n";
		cout << "\nBranch:\n";
		for (int i=0; i<MerkleBranch.Vec.size(); ++i)
			cout << "  " << MerkleBranch.Vec[i] << endl;
#endif

	}
	return m_merkleRoot.get();
}

void MinerBlock::AssemblyCoinbaseTx(RCString address) {
	MemoryStream msScript(64);
	BinaryWriter wrScript(msScript);	
	wrScript << BigInteger(Height) << byte(8);
	size_t coinb1Size = (size_t)msScript.Position;
	wrScript << int64_t(0);					// place for ExtraNonce1, ExtraNonce2;
	if (CoinbaseAux.Size) {
		ASSERT(CoinbaseAux.Size < 75);	//!!!TODO

		byte firstByte = CoinbaseAux.constData()[0];
		if (firstByte==0 || firstByte!=CoinbaseAux.Size-1)
			wrScript << byte(CoinbaseAux.Size);
		msScript.WriteBuf(CoinbaseAux);
	}
	Blob scriptIn = Blob(ConstBuf(msScript));

	MemoryStream ms(128);
	BinaryWriter wr(ms);
	wr << uint32_t(1) << byte(1) << HashValue() << uint32_t(-1);
	CoinSerialized::WriteVarInt(wr, scriptIn.Size);
	coinb1Size += int(ms.Position);
	ms.WriteBuf(scriptIn);
	wr << uint32_t(-1) << byte(1) << CoinbaseValue								// seq, NOut
		<< byte(25)																// len(PkScript)
		<< byte(0x76) << byte(0xA9) << byte(20);									// OP_DUP OP_HASH160, hash160Len
	ms.WriteBuf(ConstBuf(ConvertFromBase58(address, false).constData()+1, 20));
	wr << byte(0x88) << byte(0xAC);												//OP_EQUALVERIFY OP_CHECKSIG
	wr << uint32_t(0);		// locktime;
	Blob coinbasetxn = ms;
	Coinb1 = Blob(coinbasetxn.constData(), coinb1Size);
	Coinb2 = Blob(coinbasetxn.constData()+coinb1Size+8, coinbasetxn.Size-coinb1Size-8);
}

void MinerShare::WriteHeader(BinaryWriter& wr) const {
	base::WriteHeader(wr);
	switch (Algo) {
	case HashAlgo::Momentum:
		wr << BirthdayA << BirthdayB;
		break;
	}
}

static HashValue s_hashMax("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF");

HashValue MinerShare::GetHash() const {
	switch (Algo) {
	case HashAlgo::Sha3:
		{
			MemoryStream ms;
			base::WriteHeader(BinaryWriter(ms).Ref());
			return HashValue(SHA3<256>().ComputeHash(ConstBuf(ms)));
		}
	case HashAlgo::Metis:
		{
			MemoryStream ms;
			base::WriteHeader(BinaryWriter(ms).Ref());
			return MetisHash(ms);
		}
	case HashAlgo::Momentum:
		{
			MemoryStream ms;
			base::WriteHeader(BinaryWriter(ms).Ref());
			if (!MomentumVerify(Coin::Hash(ms), BirthdayA, BirthdayB))
				return s_hashMax;
		}
	default:
		return base::GetHash();
	}
}

HashValue MinerShare::GetHashPow() const {
	switch (Algo) {
	case HashAlgo::SCrypt:
		{
			MemoryStream ms;
			WriteHeader(BinaryWriter(ms).Ref());
			return ScryptHash(ms);
		}
		break;
	default:
		return GetHash();
	}
}

void PrimeMinerShare::WriteHeader(BinaryWriter& wr) const {
	base::WriteHeader(wr);
	CoinSerialized::WriteBlob(wr, PrimeChainMultiplier.ToBytes());	
}

uint32_t PrimeMinerShare::GetDifficulty() const {
	MemoryStream ms;
	base::WriteHeader(BinaryWriter(ms).Ref());
	HashValue h = Hash(ms);
	if (h[31] < 0x80)
		Throw(E_COIN_MINER_REJECTED);
	BigInteger origin = HashValueToBigInteger(h) * PrimeChainMultiplier;
	PrimeTester tester;				//!!!TODO move to some long-time scope
	return uint32_t(tester.ProbablePrimeChainTest(Bn(origin)).BestTypeLength().second * 0x1000000);
}


} // Coin::
