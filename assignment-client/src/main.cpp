//
//  main.cpp
//  assignment-client
//
//  Created by Stephen Birarda on 8/22/13.
//  Copyright (c) 2013 High Fidelity, Inc. All rights reserved.
//

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <Logging.h>
#include <NodeList.h>
#include <PacketHeaders.h>
#include <SharedUtil.h>
#include <VoxelServer.h>

#include "Agent.h"
#include "Assignment.h"
#include "AssignmentFactory.h"
#include "audio/AudioMixer.h"
#include "avatars/AvatarMixer.h"

const long long ASSIGNMENT_REQUEST_INTERVAL_USECS = 1 * 1000 * 1000;
const char PARENT_TARGET_NAME[] = "assignment-client-monitor";
const char CHILD_TARGET_NAME[] = "assignment-client";

pid_t* childForks = NULL;
sockaddr_in customAssignmentSocket = {};
int numForks = 0;
Assignment::Type overiddenAssignmentType = Assignment::AllTypes;
const char* assignmentPool = NULL;

int argc = 0;
char** argv = NULL;

void childClient() {
    QCoreApplication application(::argc, ::argv);
    
    // set the logging target to the the CHILD_TARGET_NAME
    Logging::setTargetName(CHILD_TARGET_NAME);
    
    // create a NodeList as an unassigned client
    NodeList* nodeList = NodeList::createInstance(NODE_TYPE_UNASSIGNED);
    
    // set the custom assignment socket if we have it
    if (customAssignmentSocket.sin_addr.s_addr != 0) {
        nodeList->setAssignmentServerSocket((sockaddr*) &customAssignmentSocket);
    }
    
    // change the timeout on the nodelist socket to be as often as we want to re-request
    nodeList->getNodeSocket()->setBlockingReceiveTimeoutInUsecs(ASSIGNMENT_REQUEST_INTERVAL_USECS);
    
    timeval lastRequest = {};
    
    unsigned char packetData[MAX_PACKET_SIZE];
    ssize_t receivedBytes = 0;
    
    sockaddr_in senderSocket = {};
    
    // create a request assignment, accept assignments defined by the overidden type
    Assignment requestAssignment(Assignment::RequestCommand, ::overiddenAssignmentType, ::assignmentPool);
    
    qDebug() << "Waiting for assignment -" << requestAssignment << "\n";
    
    while (true) {
        if (usecTimestampNow() - usecTimestamp(&lastRequest) >= ASSIGNMENT_REQUEST_INTERVAL_USECS) {
            gettimeofday(&lastRequest, NULL);
            
            // if we're here we have no assignment, so send a request
            nodeList->sendAssignment(requestAssignment);
        }
        
        if (nodeList->getNodeSocket()->receive((sockaddr*) &senderSocket, packetData, &receivedBytes) &&
            (packetData[0] == PACKET_TYPE_DEPLOY_ASSIGNMENT || packetData[0] == PACKET_TYPE_CREATE_ASSIGNMENT)
            && packetVersionMatch(packetData)) {
            
            // construct the deployed assignment from the packet data
            Assignment* deployedAssignment = AssignmentFactory::unpackAssignment(packetData, receivedBytes);
            
            qDebug() << "Received an assignment -" << *deployedAssignment << "\n";
            
            // switch our nodelist domain IP and port to whoever sent us the assignment
            if (packetData[0] == PACKET_TYPE_CREATE_ASSIGNMENT) {
                
                nodeList->setDomainIP(QHostAddress((sockaddr*) &senderSocket));
                nodeList->setDomainPort(ntohs(senderSocket.sin_port));
                
                nodeList->setOwnerUUID(deployedAssignment->getUUID());
                
                qDebug("Destination IP for assignment is %s\n", nodeList->getDomainIP().toString().toStdString().c_str());
                
                // run the deployed assignment
                deployedAssignment->run();
            } else {
                qDebug("Received a bad destination socket for assignment.\n");
            }
            
            qDebug("Assignment finished or never started - waiting for new assignment\n");
            
            // delete the deployedAssignment
            delete deployedAssignment;
            
            // reset our NodeList by switching back to unassigned and clearing the list
            nodeList->setOwnerType(NODE_TYPE_UNASSIGNED);
            nodeList->reset();
            
            // set the NodeList socket back to blocking
            nodeList->getNodeSocket()->setBlocking(true);
            
            // reset the logging target to the the CHILD_TARGET_NAME
            Logging::setTargetName(CHILD_TARGET_NAME);
        }
    }
}

void sigchldHandler(int sig) {
    pid_t processID;
    int status;
    
    while ((processID = waitpid(-1, &status, WNOHANG)) != -1) {
        if (processID == 0) {
            // there are no more children to process, break out of here
            break;
        }
        
        int newForkProcessID = 0;
        
        // find the dead process in the array of child forks
        for (int i = 0; i < ::numForks; i++) {
            if (::childForks[i] == processID) {
                
                newForkProcessID = fork();
                if (newForkProcessID == 0) {
                    // this is the child, call childClient
                    childClient();
                    
                    // break out so we don't fork bomb
                    break;
                } else {
                    // this is the parent, replace the dead process with the new one
                    ::childForks[i] = newForkProcessID;
                   
                    qDebug("Replaced dead %d with new fork %d\n", processID, newForkProcessID);
                    
                    break;
                }
            }
        }
    }
    
}

void parentMonitor() {
    
    struct sigaction sa;
    
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchldHandler;
    
    sigaction(SIGCHLD, &sa, NULL);
    
    pid_t childID = 0;
    
    // don't bail until all children have finished
    while ((childID = waitpid(-1, NULL, 0))) {
        if (errno == ECHILD) {
            break;
        }
    }
    
    // delete the array of pid_t holding the forked process IDs
    delete[] ::childForks;
}

int main(int argc, char* argv[]) {
    ::argc = argc;
    ::argv = argv;
    
    setvbuf(stdout, NULL, _IOLBF, 0);
    
    // use the verbose message handler in Logging
    qInstallMessageHandler(Logging::verboseMessageHandler);
    
    // start the Logging class with the parent's target name
    Logging::setTargetName(PARENT_TARGET_NAME);
    
    const char CUSTOM_ASSIGNMENT_SERVER_HOSTNAME_OPTION[] = "-a";
    const char CUSTOM_ASSIGNMENT_SERVER_PORT_OPTION[] = "-p";
    
    // grab the overriden assignment-server hostname from argv, if it exists
    const char* customAssignmentServerHostname = getCmdOption(argc, (const char**)argv, CUSTOM_ASSIGNMENT_SERVER_HOSTNAME_OPTION);
    const char* customAssignmentServerPortString = getCmdOption(argc,(const char**)argv, CUSTOM_ASSIGNMENT_SERVER_PORT_OPTION);
    
    if (customAssignmentServerHostname || customAssignmentServerPortString) {
        
        // set the custom port or default if it wasn't passed
        unsigned short assignmentServerPort = customAssignmentServerPortString
            ? atoi(customAssignmentServerPortString) : DEFAULT_DOMAIN_SERVER_PORT;
        
        // set the custom hostname or default if it wasn't passed
        if (!customAssignmentServerHostname) {
            customAssignmentServerHostname = LOCAL_ASSIGNMENT_SERVER_HOSTNAME;
        }
        
        ::customAssignmentSocket = socketForHostnameAndHostOrderPort(customAssignmentServerHostname, assignmentServerPort);
    }
    
    const char ASSIGNMENT_TYPE_OVVERIDE_OPTION[] = "-t";
    const char* assignmentTypeString = getCmdOption(argc, (const char**)argv, ASSIGNMENT_TYPE_OVVERIDE_OPTION);
    
    if (assignmentTypeString) {
        // the user is asking to only be assigned to a particular type of assignment
        // so set that as the ::overridenAssignmentType to be used in requests
        ::overiddenAssignmentType = (Assignment::Type) atoi(assignmentTypeString);
    }
    
    const char ASSIGNMENT_POOL_OPTION[] = "--pool";
    ::assignmentPool = getCmdOption(argc, (const char**) argv, ASSIGNMENT_POOL_OPTION);

    const char* NUM_FORKS_PARAMETER = "-n";
    const char* numForksString = getCmdOption(argc, (const char**)argv, NUM_FORKS_PARAMETER);
    
    int processID = 0;
    
    if (numForksString) {
        ::numForks = atoi(numForksString);
        qDebug("Starting %d assignment clients\n", ::numForks);
        
        ::childForks = new pid_t[::numForks];
        
        // fire off as many children as we need (this is one less than the parent since the parent will run as well)
        for (int i = 0; i < ::numForks; i++) {
            processID = fork();
            
            if (processID == 0) {
                // this is in one of the children, break so we don't start a fork bomb
                break;
            } else {
                // this is in the parent, save the ID of the forked process
                childForks[i] = processID;
            }
        }
    }
    
    if (processID == 0 || ::numForks == 0) {
        childClient();
    } else {
        parentMonitor();
    }
}