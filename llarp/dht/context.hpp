#ifndef LLARP_DHT_CONTEXT_HPP
#define LLARP_DHT_CONTEXT_HPP

#include <dht.h>
#include <dht/bucket.hpp>
#include <dht/key.hpp>
#include <dht/message.hpp>
#include <dht/messages/findintro.hpp>
#include <dht/node.hpp>
#include <service/IntroSet.hpp>
#include <time.hpp>

#include <set>

namespace llarp
{
  struct Router;

  namespace dht
  {
    struct TXOwner
    {
      Key_t node;
      uint64_t txid = 0;

      TXOwner() = default;

      TXOwner(const Key_t& k, uint64_t id) : node(k), txid(id)
      {
      }

      bool
      operator==(const TXOwner& other) const
      {
        return txid == other.txid && node == other.node;
      }
      bool
      operator<(const TXOwner& other) const
      {
        return txid < other.txid || node < other.node;
      }

      struct Hash
      {
        std::size_t
        operator()(TXOwner const& o) const noexcept
        {
          std::size_t sz2;
          memcpy(&sz2, &o.node[0], sizeof(std::size_t));
          return o.txid ^ (sz2 << 1);
        }
      };
    };

    struct Context;

    template < typename K, typename V >
    struct TX
    {
      TX(const TXOwner& asker, const K& k, Context* p)
          : target(k), whoasked(asker)
      {
        parent = p;
      }

      virtual ~TX(){};

      K target;
      Context* parent;
      std::set< Key_t > peersAsked;
      std::vector< V > valuesFound;
      TXOwner whoasked;

      virtual bool
      Validate(const V& value) const = 0;

      void
      OnFound(const Key_t askedPeer, const V& value)
      {
        peersAsked.insert(askedPeer);
        if(Validate(value))
          valuesFound.push_back(value);
      }

      virtual void
      Start(const TXOwner& peer) = 0;

      virtual bool
      GetNextPeer(Key_t& next, const std::set< Key_t >& exclude) = 0;

      virtual void
      DoNextRequest(const Key_t& peer) = 0;

      /// return true if we want to persist this tx
      bool
      AskNextPeer(const Key_t& prevPeer, const std::unique_ptr< Key_t >& next)
      {
        peersAsked.insert(prevPeer);
        Key_t peer;
        if(next)
        {
          // explicit next peer provided
          peer = next->data();
        }
        else if(!GetNextPeer(peer, peersAsked))
        {
          // no more peers
          llarp::LogInfo("no more peers for request asking for", target);
          return false;
        }

        const Key_t targetKey = target.data();
        if((prevPeer ^ targetKey) < (peer ^ targetKey))
        {
          // next peer is not closer
          llarp::LogInfo("next peer ", peer, " is not closer to ", target,
                         " than ", prevPeer);
          return false;
        }
        else
          peersAsked.insert(peer);
        DoNextRequest(peer);
        return true;
      }

      virtual void
      SendReply() = 0;
    };

    using IntroSetLookupHandler =
        std::function< void(const std::vector< service::IntroSet >&) >;

    using RouterLookupHandler =
        std::function< void(const std::vector< RouterContact >&) >;

    struct Context
    {
      Context();
      ~Context();

      llarp::Crypto*
      Crypto();

      /// on behalf of whoasked request introset for target from dht router with
      /// key askpeer
      void
      LookupIntroSetRecursive(const service::Address& target,
                              const Key_t& whoasked, uint64_t whoaskedTX,
                              const Key_t& askpeer, uint64_t R,
                              IntroSetLookupHandler result = nullptr);

      void
      LookupIntroSetIterative(const service::Address& target,
                              const Key_t& whoasked, uint64_t whoaskedTX,
                              const Key_t& askpeer,
                              IntroSetLookupHandler result = nullptr);

      /// on behalf of whoasked request router with public key target from dht
      /// router with key askpeer
      void
      LookupRouterRecursive(const RouterID& target, const Key_t& whoasked,
                            uint64_t whoaskedTX, const Key_t& askpeer,
                            RouterLookupHandler result = nullptr);

      bool
      LookupRouter(const RouterID& target, RouterLookupHandler result)
      {
        Key_t askpeer;
        if(!nodes->FindClosest(target.data(), askpeer))
          return false;
        LookupRouterRecursive(target, OurKey(), 0, askpeer, result);
        return true;
      }

      bool
      HasRouterLookup(const RouterID& target) const
      {
        return pendingRouterLookups.HasLookupFor(target);
      }

      /// on behalf of whoasked request introsets with tag from dht router with
      /// key askpeer with Recursion depth R
      void
      LookupTagRecursive(const service::Tag& tag, const Key_t& whoasked,
                         uint64_t whoaskedTX, const Key_t& askpeer, uint64_t R);

      /// issue dht lookup for tag via askpeer and send reply to local path
      void
      LookupTagForPath(const service::Tag& tag, uint64_t txid,
                       const llarp::PathID_t& path, const Key_t& askpeer);

      /// issue dht lookup for router via askpeer and send reply to local path
      void
      LookupRouterForPath(const RouterID& target, uint64_t txid,
                          const llarp::PathID_t& path, const Key_t& askpeer);

      /// issue dht lookup for introset for addr via askpeer and send reply to
      /// local path
      void
      LookupIntroSetForPath(const service::Address& addr, uint64_t txid,
                            const llarp::PathID_t& path, const Key_t& askpeer);

      /// send a dht message to peer, if keepalive is true then keep the session
      /// with that peer alive for 10 seconds
      void
      DHTSendTo(const byte_t* peer, IMessage* msg, bool keepalive = true);

      /// get routers closest to target excluding requester
      bool
      HandleExploritoryRouterLookup(
          const Key_t& requester, uint64_t txid, const RouterID& target,
          std::vector< std::unique_ptr< IMessage > >& reply);

      std::set< service::IntroSet >
      FindRandomIntroSetsWithTagExcluding(
          const service::Tag& tag, size_t max = 2,
          const std::set< service::IntroSet >& excludes = {});

      /// handle rc lookup from requester for target
      void
      LookupRouterRelayed(const Key_t& requester, uint64_t txid,
                          const Key_t& target, bool recursive,
                          std::vector< std::unique_ptr< IMessage > >& replies);

      /// relay a dht messeage from a local path to the main network
      bool
      RelayRequestForPath(const llarp::PathID_t& localPath,
                          const IMessage* msg);

      /// send introset to peer from source with S counter and excluding peers
      void
      PropagateIntroSetTo(const Key_t& source, uint64_t sourceTX,
                          const service::IntroSet& introset, const Key_t& peer,
                          uint64_t S, const std::set< Key_t >& exclude);

      /// initialize dht context and explore every exploreInterval milliseconds
      void
      Init(const Key_t& us, llarp::Router* router,
           llarp_time_t exploreInterval);

      /// get localally stored introset by service address
      const llarp::service::IntroSet*
      GetIntroSetByServiceAddress(const llarp::service::Address& addr) const;

      static void
      handle_cleaner_timer(void* user, uint64_t orig, uint64_t left);

      static void
      handle_explore_timer(void* user, uint64_t orig, uint64_t left);

      /// explore dht for new routers
      void
      Explore(size_t N = 3);

      llarp::Router* router = nullptr;
      // for router contacts
      Bucket< RCNode >* nodes = nullptr;

      // for introduction sets
      Bucket< ISNode >* services = nullptr;
      bool allowTransit          = false;

      const byte_t*
      OurKey() const
      {
        return ourKey;
      }

      template < typename K, typename V, typename K_Hash,
                 llarp_time_t requestTimeoutMS = 5000UL >
      struct TXHolder
      {
        // tx who are waiting for a reply for each key
        std::unordered_multimap< K, TXOwner, K_Hash > waiting;
        // tx timesouts by key
        std::unordered_map< K, llarp_time_t, K_Hash > timeouts;
        // maps remote peer with tx to handle reply from them
        std::unordered_map< TXOwner, std::unique_ptr< TX< K, V > >,
                            TXOwner::Hash >
            tx;

        const TX< K, V >*
        GetPendingLookupFrom(const TXOwner& owner) const
        {
          auto itr = tx.find(owner);
          if(itr == tx.end())
            return nullptr;
          else
            return itr->second.get();
        }

        bool
        HasLookupFor(const K& target) const
        {
          return timeouts.find(target) != timeouts.end();
        }

        bool
        HasPendingLookupFrom(const TXOwner& owner) const
        {
          return GetPendingLookupFrom(owner) != nullptr;
        }

        void
        NewTX(const TXOwner& askpeer, const TXOwner& whoasked, const K& k,
              TX< K, V >* t)
        {
          (void)whoasked;
          tx.emplace(askpeer, std::unique_ptr< TX< K, V > >(t));
          auto count = waiting.count(k);
          waiting.insert(std::make_pair(k, askpeer));

          auto itr = timeouts.find(k);
          if(itr == timeouts.end())
          {
            timeouts.insert(
                std::make_pair(k, time_now_ms() + requestTimeoutMS));
          }
          if(count == 0)
            t->Start(askpeer);
        }

        /// mark tx as not fond
        void
        NotFound(const TXOwner& from, const std::unique_ptr< Key_t >& next)
        {
          bool sendReply = true;
          auto txitr     = tx.find(from);
          if(txitr == tx.end())
            return;

          // ask for next peer
          if(txitr->second->AskNextPeer(from.node, next))
            sendReply = false;

          llarp::LogWarn("Target key ", txitr->second->target);
          Inform(from, txitr->second->target, {}, sendReply, sendReply);
        }

        void
        Found(const TXOwner& from, const K& k, const std::vector< V >& values)
        {
          Inform(from, k, values, true);
        }

        /// inform all watches for key of values found
        void
        Inform(TXOwner from, K key, std::vector< V > values,
               bool sendreply = false, bool removeTimeouts = true)
        {
          auto range = waiting.equal_range(key);
          auto itr   = range.first;
          while(itr != range.second)
          {
            auto txitr = tx.find(itr->second);
            if(txitr != tx.end())
            {
              for(const auto& value : values)
                txitr->second->OnFound(from.node, value);
              if(sendreply)
              {
                txitr->second->SendReply();
                tx.erase(txitr);
              }
            }
            ++itr;
          }

          if(sendreply)
            waiting.erase(key);

          if(removeTimeouts)
            timeouts.erase(key);
        }

        void
        Expire(llarp_time_t now)
        {
          auto itr = timeouts.begin();
          while(itr != timeouts.end())
          {
            if(now > itr->second && now - itr->second >= requestTimeoutMS)
            {
              Inform(TXOwner{}, itr->first, {}, true, false);
              itr = timeouts.erase(itr);
            }
            else
              ++itr;
          }
        }
      };

      TXHolder< service::Address, service::IntroSet, service::Address::Hash >
          pendingIntrosetLookups;

      TXHolder< service::Tag, service::IntroSet, service::Tag::Hash >
          pendingTagLookups;

      TXHolder< RouterID, RouterContact, RouterID::Hash > pendingRouterLookups;

      TXHolder< RouterID, RouterID, RouterID::Hash > pendingExploreLookups;

      uint64_t
      NextID()
      {
        return ++ids;
      }

      llarp_time_t
      Now();

      void
      ExploreNetworkVia(const Key_t& peer);

     private:
      void
      ScheduleCleanupTimer();

      void
      CleanupTX();

      uint64_t ids;

      Key_t ourKey;
    };  // namespace llarp
  }     // namespace dht
}  // namespace llarp

struct llarp_dht_context
{
  llarp::dht::Context impl;
  llarp::Router* parent;
  llarp_dht_context(llarp::Router* router);
};

#endif
