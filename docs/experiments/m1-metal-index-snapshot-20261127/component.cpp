#include "pica_metal_index_scan_memo.h"
#include "pica_metal_index_snapshot_proof.h"
#include <vector>
#include <iostream>
#include <stdexcept>
using namespace Cara;
static int checks=0;
void require(bool yes,const char* what){++checks;if(!yes)throw std::runtime_error(what);}
template<class T> auto view(const T& v){return std::as_bytes(std::span(v));}
int main(){
 ExactMetalIndexScanMemo memo; ExactMetalIndexScanMemo::Key k{0x20001000,6,1};
 ExactMetalIndexScanMemo::Result result{0,5,6};std::vector<std::uint8_t> data{0,1,2,3,4,5};MetalIndexSnapshotLease lease;
 require(!memo.Lookup(k,data,&lease)&&!lease,"miss cannot mint lease");
 memo.Store(k,data,result);require(memo.Lookup(k,data,&lease)==result&&bool(lease),"exact lookup mints lease");
 auto first=lease;require(lease->Bytes()==data.size(),"lease byte bound");
 require(memo.Lookup(k,data,&lease)==result&&lease==first,"unchanged payload stable identity");
 auto image=std::make_shared<std::vector<std::uint16_t>>(256);(*image)[0]=4;
 std::memcpy(image->data()+2,data.data(),data.size());
 MetalVerifiedIndexRead read;
 require(bool(memo.Lookup(k,{reinterpret_cast<const std::uint8_t*>(image->data()+2),data.size()},&lease,&read)),"capture payload rechecked with provenance");
 auto proof=MetalIndexUploadProof::Bind(read,image);require(bool(proof),"valid padded capture");
 auto key=proof->KeyFor(view(*image));require(bool(key),"key bound to captured view");
 auto copied=*image;require(!proof->KeyFor(view(copied)),"other allocation needs its own binding");
 require(!proof->KeyFor(view(*image).first(511)),"wrong image size rejects");
 require(!MetalIndexUploadProof::Bind(MetalVerifiedIndexRead{},image),"no lease rejects");
 require(!MetalIndexUploadProof::Bind(read,{}),"no capture owner rejects");
 (*image)[0]^=1;require(!proof->KeyFor(view(*image)),"changed header cannot keep key");(*image)[0]^=1;
 auto badpad=std::make_shared<std::vector<std::uint16_t>>(*image);badpad->back()=1;MetalVerifiedIndexRead padread;
 require(bool(memo.Lookup(k,{reinterpret_cast<const std::uint8_t*>(badpad->data()+2),data.size()},nullptr,&padread)),"padding negative has valid payload provenance");
 require(!MetalIndexUploadProof::Bind(padread,badpad),"nonzero padding rejects");
 auto oversized=std::make_shared<std::vector<std::uint16_t>>(512);require(!MetalIndexUploadProof::Bind(read,oversized),"unbounded padding rejects");
 auto shortimage=std::make_shared<std::vector<std::uint16_t>>(2);require(!MetalIndexUploadProof::Bind(read,shortimage),"truncated payload rejects");
 data[3]=99;require(!memo.Lookup(k,data,&lease)&&!lease,"changed payload clears outgoing lease");
 memo.Store(k,data,{0,99,6});require(bool(memo.Lookup(k,data,&lease))&&lease!=first,"replace retires identity before overwrite");
 auto image2=std::make_shared<std::vector<std::uint16_t>>(*image);std::memcpy(image2->data()+2,data.data(),data.size());
 MetalVerifiedIndexRead read2;
 require(bool(memo.Lookup(k,{reinterpret_cast<const std::uint8_t*>(image2->data()+2),data.size()},&lease,&read2)),"changed payload current provenance");
 auto secondproof=MetalIndexUploadProof::Bind(read2,image2);require(secondproof&&secondproof->KeyFor(view(*image2))!=key,"new revision cannot match retained GPU key");
 auto invalid=k;invalid.width=3;require(!memo.Lookup(invalid,data,&lease)&&!lease,"invalid width clears stale lease");
 auto overflow=k;overflow.address=0xffffffff;require(!memo.Lookup(overflow,data,&lease)&&!lease,"overflow clears stale lease");
 for(unsigned i=0;i<140;++i){auto q=k;q.address+=0x1000ull*(i+1);memo.Store(q,data,{0,99,6});}
 require(memo.SmallStats().entries==128,"memo bound preserved");require(!memo.Lookup(k,data,&lease)&&!lease,"evicted snapshot cannot hit");
 memo.Store(k,data,{0,99,6});require(bool(memo.Lookup(k,data,&lease))&&lease!=first,"reinsert cannot impersonate retained token");
 std::weak_ptr<std::vector<std::uint16_t>> weak=image;image.reset();require(!weak.expired(),"capture proof pins owner");proof.reset();require(weak.expired(),"GPU key does not retain capture storage");
 ExactMetalIndexScanMemo fresh;fresh.Store(k,data,{0,99,6});MetalIndexSnapshotLease freshlease;
 require(bool(fresh.Lookup(k,data,&freshlease))&&freshlease!=lease,"fresh memo identity distinct");
 require(!fresh.Lookup(k,std::span<const std::uint8_t>(data).first(5),&freshlease)&&!freshlease,"partial read cannot borrow full proof");
 struct Holder {std::vector<std::uint16_t> indices;};auto holder=std::make_shared<Holder>();holder->indices=*image2;
 std::weak_ptr<Holder> held=holder;auto alias=std::shared_ptr<const std::vector<std::uint16_t>>(holder,&holder->indices);
 MetalVerifiedIndexRead read3;
 require(bool(memo.Lookup(k,{reinterpret_cast<const std::uint8_t*>(alias->data()+2),data.size()},&lease,&read3)),"alias owner current provenance");
 auto bound=MetalIndexUploadProof::Bind(read3,alias);require(bool(bound),"typed alias owns actual capture vector");holder.reset();alias.reset();require(!held.expired(),"alias pins binding storage");
 bound.reset();require(held.expired(),"releasing upload proof releases storage");
 auto unrelated=std::make_shared<std::vector<std::uint16_t>>(*image2);
 require(!MetalIndexUploadProof::Bind(read2,unrelated),"proof cannot cross byte-equal allocation");
 require(!memo.Lookup(k,std::span<const std::uint8_t>(data).first(5),nullptr,&read2)&&!read2,"failed read clears old provenance");
 auto changed_owner=std::make_shared<std::vector<std::uint16_t>>(*image2);MetalVerifiedIndexRead extentread;
 require(bool(memo.Lookup(k,{reinterpret_cast<const std::uint8_t*>(changed_owner->data()+2),data.size()},nullptr,&extentread)),"extent negative valid provenance");
 auto extentproof=MetalIndexUploadProof::Bind(extentread,changed_owner);require(bool(extentproof),"extent negative starts valid");
 changed_owner->resize(512);require(!extentproof->KeyFor(view(*changed_owner)),"owner reallocation invalidates view before dereference");
 std::cout<<"PASS "<<checks<<" ownership/lifetime/negative checks\n";
}
