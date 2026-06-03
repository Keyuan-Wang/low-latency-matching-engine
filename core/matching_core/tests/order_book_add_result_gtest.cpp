#include "matching/order_book.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <deque>
#include <iterator>
#include <map>
#include <ostream>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

std::ostream& operator<<(std::ostream& os, matching::ErrorCode c) {
    switch(c) {
    case matching::ErrorCode::Success: return os<<"Success";
    case matching::ErrorCode::InvalidQuantity: return os<<"InvalidQuantity";
    case matching::ErrorCode::DuplicateOrderId: return os<<"DuplicateOrderId";
    case matching::ErrorCode::UnknownOrderId: return os<<"UnknownOrderId";
    case matching::ErrorCode::PendingCancelExists: return os<<"PendingCancelExists";
    case matching::ErrorCode::MarketRemainderCancelled: return os<<"MarketRemainderCancelled";
    } return os<<"ErrorCode("<<static_cast<int>(c)<<")";
}
namespace {
struct RO{uint64_t id=0;int64_t p=0;uint64_t q=0;};
class Ref{
public:
matching::AddResult add_limit(uint64_t o,matching::Side s,int64_t p,uint64_t q,uint64_t){
matching::AddResult r{};r.initial_quantity=q;if(q==0){r.code=matching::ErrorCode::InvalidQuantity;return r;}
uint64_t rem=q;if(s==matching::Side::Buy)mlt(s,p,rem,as_,r);else mlt(s,p,rem,bs_,r);
r.remaining_quantity=rem;if(rem==0){r.code=matching::ErrorCode::Success;return r;}
rst(o,s,p,rem);r.code=matching::ErrorCode::Success;return r;}
matching::AddResult add_mkt(uint64_t o,matching::Side s,uint64_t q,uint64_t){
matching::AddResult r{};r.initial_quantity=q;if(q==0){r.code=matching::ErrorCode::InvalidQuantity;return r;}
uint64_t rem=q;if(s==matching::Side::Buy)mmk(rem,as_,r);else mmk(rem,bs_,r);
r.remaining_quantity=rem;r.code=(rem==0)?matching::ErrorCode::Success:matching::ErrorCode::MarketRemainderCancelled;return r;}
matching::AddResult md(uint64_t o,matching::Side s,int64_t p,uint64_t q,uint64_t t){cl(o);return add_limit(o,s,p,q,t);}
bool cl(uint64_t o){return cf(o,bs_)||cf(o,as_);}
private:
std::map<int64_t,std::deque<RO>,std::greater<>> bs_{};
std::map<int64_t,std::deque<RO>,std::less<>> as_{};
template<typename B>void mlt(matching::Side sd,int64_t lp,uint64_t& r,B& ob,matching::AddResult& o){
while(r>0&&!ob.empty()){auto bp=ob.begin()->first;if(!((sd==matching::Side::Buy)?(lp>=bp):(lp<=bp)))break;mlv(r,ob,o);}}
template<typename B>void mmk(uint64_t& r,B& ob,matching::AddResult& o){while(r>0&&!ob.empty())mlv(r,ob,o);}
template<typename B>void mlv(uint64_t& r,B& ob,matching::AddResult& o){
auto li=ob.begin();auto& q=li->second;while(r>0&&!q.empty()){auto& m=q.front();auto f=std::min(r,m.q);
o.trades.push_back(matching::Trade{0,m.id,m.p,f});m.q-=f;r-=f;o.filled_quantity+=f;if(m.q==0)q.pop_front();}
if(q.empty())ob.erase(li);}
void rst(uint64_t o,matching::Side s,int64_t p,uint64_t q){if(s==matching::Side::Buy)bs_[p].push_back({o,p,q});else as_[p].push_back({o,p,q});}
template<typename B>bool cf(uint64_t o,B& b){for(auto li=b.begin();li!=b.end();++li){auto& q=li->second;auto oi=std::find_if(q.begin(),q.end(),[&](const RO& x){return x.id==o;});if(oi==q.end())continue;q.erase(oi);if(q.empty())b.erase(li);return true;}return false;}
};
void eqr(const matching::AddResult& a,const matching::AddResult& e){
EXPECT_EQ(a.code,e.code);EXPECT_EQ(a.initial_quantity,e.initial_quantity);
EXPECT_EQ(a.filled_quantity,e.filled_quantity);EXPECT_EQ(a.remaining_quantity,e.remaining_quantity);
ASSERT_EQ(a.trades.size(),e.trades.size());
for(std::size_t i=0;i<a.trades.size();++i){SCOPED_TRACE("t"+std::to_string(i));EXPECT_EQ(a.trades[i].taker_order_id,e.trades[i].taker_order_id);EXPECT_EQ(a.trades[i].maker_order_id,e.trades[i].maker_order_id);EXPECT_EQ(a.trades[i].price,e.trades[i].price);EXPECT_EQ(a.trades[i].quantity,e.trades[i].quantity);}
}
void aft(const matching::AddResult& r,std::unordered_map<uint64_t,uint64_t>& l){for(auto& t:r.trades){auto it=l.find(t.maker_order_id);if(it==l.end())continue;if(t.quantity>=it->second)l.erase(it);else it->second-=t.quantity;}}
class H{
public:
explicit H(std::size_t c=200000):a_(c){}
void al(uint64_t o,matching::Side s,int64_t p,uint64_t q,uint64_t t){auto e=ref_.add_limit(o,s,p,q,t);auto x=a_.add_limit_order(o,s,p,q,t);eqr(x,e);ap(o,x);}
void am(uint64_t o,matching::Side s,uint64_t q,uint64_t t){auto e=ref_.add_mkt(o,s,q,t);auto x=a_.add_market_order(o,s,q,t);eqr(x,e);aft(x,l_);}
void mo(uint64_t o,matching::Side s,int64_t p,uint64_t q,uint64_t t){auto e=ref_.md(o,s,p,q,t);auto x=a_.modify_order(o,s,p,q,t);eqr(x,e);ap(o,x);}
void cl(uint64_t o){ref_.cl(o);a_.cancel_order(o);l_.erase(o);}
void rnd(uint64_t sd=42,std::size_t n=5000){std::mt19937 rng(sd);uint64_t nid=1;std::unordered_map<uint64_t,matching::Side>kn;for(std::size_t i=0;i<n;++i){if(kn.empty()||rng()%4!=0){auto s=(rng()%2==0)?matching::Side::Buy:matching::Side::Sell;auto p=1000+static_cast<int64_t>(rng()%100)*(s==matching::Side::Buy?-1:1);auto q=1+(rng()%10);auto o=nid++;al(o,s,p,q,o);kn[o]=s;}else{auto it=kn.begin();std::advance(it,rng()%kn.size());cl(it->first);kn.erase(it);}}}
private:
matching::OrderBook a_;Ref ref_;std::unordered_map<uint64_t,uint64_t>l_;
void ap(uint64_t o,const matching::AddResult& x){aft(x,l_);if(x.code==matching::ErrorCode::Success&&x.remaining_quantity>0)l_[o]=x.remaining_quantity;}
};
TEST(OrderBookAddResultTest,add_limit_rest){H h;h.al(1,matching::Side::Buy,100,10,1);h.al(2,matching::Side::Sell,200,5,2);}
TEST(OrderBookAddResultTest,add_limit_cross_partial){H h;h.al(10,matching::Side::Sell,100,5,1);h.al(11,matching::Side::Buy,100,12,2);}
TEST(OrderBookAddResultTest,add_limit_cross_full){H h;h.al(10,matching::Side::Sell,100,5,1);h.al(11,matching::Side::Buy,105,5,2);}
TEST(OrderBookAddResultTest,add_limit_cross_multi){H h;h.al(10,matching::Side::Sell,100,5,1);h.al(11,matching::Side::Sell,101,5,2);h.al(12,matching::Side::Buy,101,10,3);}
TEST(OrderBookAddResultTest,add_market_sweep){H h;h.al(101,matching::Side::Sell,100,5,1);h.al(102,matching::Side::Sell,101,5,2);h.am(500,matching::Side::Buy,10,3);}
TEST(OrderBookAddResultTest,add_market_remainder){H h;h.al(101,matching::Side::Sell,100,5,1);h.am(500,matching::Side::Buy,10,2);}
TEST(OrderBookAddResultTest,modify_resting){H h;h.al(1,matching::Side::Buy,100,10,1);h.mo(1,matching::Side::Buy,99,5,2);}
TEST(OrderBookAddResultTest,cancel_resting){H h;h.al(1,matching::Side::Buy,100,10,1);h.cl(1);}
TEST(OrderBookAddResultTest,duplicate_order_id){H h;h.al(1,matching::Side::Buy,100,10,1);h.al(1,matching::Side::Sell,101,5,2);}
TEST(OrderBookAddResultTest,random_workload){H h;h.rnd(42,5000);}
}
