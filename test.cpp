#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <iostream>
#include <random>
#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>

bool running = true;

void signalHandler(int signal) {
    running = false;
}

UA_LocalizedText createLocalizedText(const char* locale, const char* text) {
    UA_LocalizedText lt;
    lt.locale = UA_STRING_ALLOC(locale);
    lt.text = UA_STRING_ALLOC(text);
    return lt;
}

// Функция для создания QualifiedName
UA_QualifiedName createQualifiedName(UA_UInt16 nsIndex, const char* name) {
    UA_QualifiedName qn;
    qn.namespaceIndex = nsIndex;
    qn.name = UA_STRING_ALLOC(name);
    return qn;
}

int main(){
    //объявление сервера
    UA_Server *server = UA_Server_new();
    UA_ServerConfig_setDefault(UA_Server_getConfig(server));

    UA_UInt16 nsIdx = UA_Server_addNamespace(server, "MyNamespace");

    UA_ObjectAttributes voltmeterAttr = UA_ObjectAttributes_default;
    voltmeterAttr.displayName = createLocalizedText("en-US", "Voltmeter");
    
    UA_NodeId voltmeterId;
    UA_StatusCode status = UA_Server_addObjectNode(server, UA_NODEID_NUMERIC(nsIdx, 0),
                                                  UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                                                  UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                                                  createQualifiedName(nsIdx, "Voltmeter"),
                                                  UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE),
                                                  voltmeterAttr, NULL, &voltmeterId);
    
    UA_LocalizedText_clear(&voltmeterAttr.displayName);
    //Создание переменной "напряжение"
    UA_VariableAttributes voltageAttr = UA_VariableAttributes_default;
        //имя переменной
    voltageAttr.displayName = createLocalizedText("en-US", "Voltage");
        //тип переменной(в данном случае double)
    voltageAttr.dataType = UA_TYPES[UA_TYPES_DOUBLE].typeId;
        //указание тип переменной скаляр/массив (В данном случае скаляр)
    voltageAttr.valueRank = UA_VALUERANK_SCALAR;
        //указание прав доступа к переменной (в данном случае доступ к чтению и записи)
    voltageAttr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
        //созданние переменной начального значения 0.0
    double initialValue = 0.0;
        //записть 
    UA_Variant_setScalar(&voltageAttr.value, &initialValue, &UA_TYPES[UA_TYPES_DOUBLE]);
    UA_NodeId voltageId;
    UA_Server_addVariableNode(server, UA_NODEID_NUMERIC(nsIdx, 1),
                          voltmeterId,
                          UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
                          createQualifiedName(nsIdx, "Voltage"),
                          UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
                          voltageAttr, NULL, &voltageId);

    UA_LocalizedText_clear(&voltageAttr.displayName);
    
    //Рандомайзер
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<double> dist(190.0, 240.0);
    //переменные 
    double V = 220.0;
    
    //Задание параметров выхода
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    //start servera
    UA_Server_run_startup(server);
    //цикл сервера
    while (running){
        V = dist(gen);

        std::cout << "\r" << V << " V" << std::flush;

        UA_Variant value;
        UA_Variant_init(&value);
        UA_Variant_setScalar(&value, &V, &UA_TYPES[UA_TYPES_DOUBLE]);
        UA_Server_writeValue(server, voltageId, value);
        
        UA_Server_run_iterate(server, false);

        std::this_thread::sleep_for(std::chrono::milliseconds(330));
        
    }
    std::cout << "\nStopping server..." << std::endl;
    //остановка сервера
    UA_Server_run_shutdown(server);
    //удаление объявленно сервера
    UA_Server_delete(server);
}