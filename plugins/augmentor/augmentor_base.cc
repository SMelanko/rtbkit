/* augmentor_base.cc

   Jeremy Barnes, 4 March 2012
   Copyright (c) 2012 Datacratic.  All rights reserved.

   Object that handles doing augmented bid requests.
*/

#include "augmentor_base.h"
#include "soa/service/zmq_utils.h"
#include "jml/arch/timers.h"
#include "jml/utils/vector_utils.h"
#include "jml/arch/futex.h"


using namespace std;
using namespace ML;


namespace RTBKIT {


/*****************************************************************************/
/* AUGMENTOR                                                                 */
/*****************************************************************************/

AugmentorBase::
AugmentorBase(const std::string & augmentorName,
          const std::string & serviceName,
          std::shared_ptr<ServiceProxies> proxies)
    : ServiceBase(serviceName, proxies),
      augmentorName(augmentorName),
      toRouters(getZmqContext())
{
}

AugmentorBase::
AugmentorBase(const std::string & augmentorName,
          const std::string & serviceName,
          ServiceBase& parent)
    : ServiceBase(serviceName, parent),
      augmentorName(augmentorName),
      toRouters(getZmqContext())
{
}

AugmentorBase::
~AugmentorBase()
{
    shutdown();
}

void
AugmentorBase::
init()
{
    toRouters.init(getServices()->config, serviceName());

    toRouters.connectHandler = [=] (const std::string & newRouter)
        {
            toRouters.sendMessage(newRouter,
                                  "CONFIG",
                                  "1.0",
                                  augmentorName);

            recordHit("messages.CONFIG");
        };

    toRouters.disconnectHandler = [=] (const std::string & oldRouter)
        {
            cerr << "disconnected from router " << oldRouter << endl;
        };

    toRouters.messageHandler = [=] (const std::string & router,
                                    const std::vector<std::string> & message)
        {
            handleRouterMessage(router, message);
        };


    toRouters.connectAllServiceProviders("rtbRouterAugmentation", "augmentors");

    addSource("AugmentorBase::toRouters", toRouters);
}

void
AugmentorBase::
start()
{
    MessageLoop::start();
}

void
AugmentorBase::
shutdown()
{
    MessageLoop::shutdown();
    toRouters.shutdown();
}

void
AugmentorBase::
configureAndWait()
{
#if 0
    sendMessage(toRouter,
                "CONFIG",
                "1.0",
                this->serviceName());
#endif

    throw ML::Exception("configureAndWait not re-implemented");
}

void
AugmentorBase::
respond(const AugmentationRequest & request, const AugmentationList & response)
{
    toRouters.sendMessage(
            request.router,
            "RESPONSE",
            "1.0",
            request.startTime,
            request.id.toString(),
            request.augmentor,
            chomp(response.toJson().toString()));

    recordHit("messages.RESPONSE");
}

void
AugmentorBase::
handleRouterMessage(const std::string & router,
                    const std::vector<std::string> & message)
{
   try {
        const std::string & type = message.at(0);
        recordHit("messages." + type);

        //cerr << "got augmentor message of type " << type << endl;
        if (type == "CONFIGOK") {
#if 0
            if (!awaitingConfig.empty()) {
                for (unsigned i = 0;  i < awaitingConfig.size();  ++i)
                    sendMessage(control, awaitingConfig[i],
                                "CONFIGOK");
            }
            awaitingConfig.clear();
#endif
        }
        else if (type == "AUGMENT") {
            const string & version = message.at(1);

            if (version != "1.0")
                throw ML::Exception("unexpected version in augment");

            AugmentationRequest request;
            request.router = router;
            request.timeAvailableMs = 0.05;
            request.augmentor = message.at(2);
            request.id = Id(message.at(3));

            const string & bidRequestSource = message.at(4);
            const string & bidRequestStr = message.at(5);

            istringstream agentsStr(message.at(6));
            ML::DB::Store_Reader reader(agentsStr);
            reader.load(request.agents);

            const string & startTimeStr = message.at(7);
            request.startTime
                = Date::fromSecondsSinceEpoch(strtod(startTimeStr.c_str(), 0));

            if (onRequest) {
                request.bidRequest.reset(
                        BidRequest::parse(bidRequestSource, bidRequestStr));

                onRequest(request);
            }
            else respond(request, AugmentationList());
        }
        else throw ML::Exception("unknown router message");

    } catch (const std::exception & exc) {
        cerr << "error handling augmentor message " << message
             << ": " << exc.what() << endl;
    }
}


/*****************************************************************************/
/* MULTI THREADED AUGMENTOR                                                   */
/*****************************************************************************/

MultiThreadedAugmentorBase::
MultiThreadedAugmentorBase(const std::string & augmentorName,
                       const std::string & serviceName,
                       std::shared_ptr<ServiceProxies> proxies)
    : AugmentorBase(augmentorName, serviceName, proxies),
      numWithInfo(0),
      ringBuffer(102400)
{
    AugmentorBase::onRequest
        = boost::bind(&MultiThreadedAugmentorBase::pushRequest, this, _1);
    numThreadsCreated = 0;
}

MultiThreadedAugmentorBase::
MultiThreadedAugmentorBase(const std::string & augmentorName,
                       const std::string & serviceName,
                       ServiceBase& parent)
    : AugmentorBase(augmentorName, serviceName, parent),
      numWithInfo(0),
      ringBuffer(102400)
{
    AugmentorBase::onRequest
        = boost::bind(&MultiThreadedAugmentorBase::pushRequest, this, _1);
    numThreadsCreated = 0;
}

MultiThreadedAugmentorBase::
~MultiThreadedAugmentorBase()
{
    shutdown();
}

void
MultiThreadedAugmentorBase::
init(int numThreads)
{
    if (numThreadsCreated)
        throw ML::Exception("double init of augmentor");

    AugmentorBase::init();

    shutdown_ = false;

    for (unsigned i = 0;  i < numThreads;  ++i)
        workers.create_thread([=] () { this->runWorker(); });

    numThreadsCreated = numThreads;
}

void
MultiThreadedAugmentorBase::
shutdown()
{
    shutdown_ = true;

    ML::memory_barrier();

    for (unsigned i = 0;  i < numThreadsCreated;  ++i)
        pushRequest(AugmentationRequest());

    workers.join_all();

    numThreadsCreated = 0;

    AugmentorBase::shutdown();
}

void
MultiThreadedAugmentorBase::
runWorker()
{
    while (!shutdown_) {
        try {
            auto req = ringBuffer.pop();
            if (shutdown_)
                return;
            doRequestImpl(req);
        } catch (const std::exception & exc) {
            std::cerr << "exception handling aug request: "
                      << exc.what() << std::endl;
        }
    }
}

void
MultiThreadedAugmentorBase::
pushRequest(const AugmentationRequest & request)
{
    ringBuffer.push(request);
}

} // namespace RTBKIT

