#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <iostream>
#include <random>
#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#endif

std::atomic<bool> running(true);

void signalHandler(int signal) {
    running = false;
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    std::cout << "OPC UA Server starting..." << std::endl;
    
    // Установка обработчиков сигналов
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // === ПРОСТАЯ ВЕРСИЯ ===
    // Создаем сервер
    UA_Server* server = UA_Server_new();
    if (!server) {
        std::cerr << "Failed to create server" << std::endl;
        return 1;
    }
    
    // Настройка сервера
    UA_ServerConfig* config = UA_Server_getConfig(server);
    UA_ServerConfig_setDefault(config);
    
    // Добавляем пространство имен
    UA_UInt16 nsIdx = UA_Server_addNamespace(server, "MyNamespace");
    
    // === Создаем переменные БЕЗ сложных атрибутов ===
    
    // Создаем переменную "Voltage"
    UA_VariableAttributes vAttr = UA_VariableAttributes_default;
    vAttr.displayName.locale = UA_STRING_NULL;
    vAttr.displayName.text = UA_STRING_ALLOC("Voltage");
    vAttr.dataType = UA_TYPES[UA_TYPES_DOUBLE].typeId;
    vAttr.valueRank = UA_VALUERANK_SCALAR;
    vAttr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    
    double initialVoltage = 220.0;
    UA_Variant_setScalar(&vAttr.value, &initialVoltage, &UA_TYPES[UA_TYPES_DOUBLE]);
    
    UA_NodeId voltageId = UA_NODEID_NUMERIC(nsIdx, 1);
    UA_NodeId parentNodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
    UA_NodeId parentReferenceNodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES);
    
    UA_QualifiedName voltageName = UA_QUALIFIEDNAME_ALLOC(nsIdx, "Voltage");
    UA_NodeId typeDefinition = UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE);
    
    UA_StatusCode status = UA_Server_addVariableNode(
        server, voltageId,
        parentNodeId,
        parentReferenceNodeId,
        voltageName,
        typeDefinition,
        vAttr, NULL, NULL);
    
    UA_String_clear(&vAttr.displayName.text);
    UA_QualifiedName_clear(&voltageName);
    
    if (status != UA_STATUSCODE_GOOD) {
        std::cerr << "Error creating voltage node: " << UA_StatusCode_name(status) << std::endl;
        UA_Server_delete(server);
        return 1;
    }
    
    // Создаем переменную "Current"
    UA_VariableAttributes cAttr = UA_VariableAttributes_default;
    cAttr.displayName.locale = UA_STRING_NULL;
    cAttr.displayName.text = UA_STRING_ALLOC("Current");
    cAttr.dataType = UA_TYPES[UA_TYPES_DOUBLE].typeId;
    cAttr.valueRank = UA_VALUERANK_SCALAR;
    cAttr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    
    double initialCurrent = 5.0;
    UA_Variant_setScalar(&cAttr.value, &initialCurrent, &UA_TYPES[UA_TYPES_DOUBLE]);
    
    UA_NodeId currentId = UA_NODEID_NUMERIC(nsIdx, 2);
    UA_QualifiedName currentName = UA_QUALIFIEDNAME_ALLOC(nsIdx, "Current");
    
    status = UA_Server_addVariableNode(
        server, currentId,
        parentNodeId,
        parentReferenceNodeId,
        currentName,
        typeDefinition,
        cAttr, NULL, NULL);
    
    UA_String_clear(&cAttr.displayName.text);
    UA_QualifiedName_clear(&currentName);
    
    if (status != UA_STATUSCODE_GOOD) {
        std::cerr << "Error creating current node: " << UA_StatusCode_name(status) << std::endl;
        UA_Server_delete(server);
        return 1;
    }
    
    // === ДОБАВЛЯЕМ СТАТИЧНУЮ ПЕРЕМЕННУЮ "ОБОРОТЫ МАХОВИКА" ===
    UA_VariableAttributes rpmAttr = UA_VariableAttributes_default;
    rpmAttr.displayName.locale = UA_STRING_NULL;
    rpmAttr.displayName.text = UA_STRING_ALLOC("Flywheel RPM");
    rpmAttr.description.locale = UA_STRING_NULL;
    rpmAttr.description.text = UA_STRING_ALLOC("Скорость вращения маховика (обороты в минуту)");
    rpmAttr.dataType = UA_TYPES[UA_TYPES_DOUBLE].typeId;
    rpmAttr.valueRank = UA_VALUERANK_SCALAR;
    rpmAttr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    rpmAttr.userAccessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    rpmAttr.minimumSamplingInterval = 0.0;
    rpmAttr.historizing = false;
    
    // Базовое значение 0
    double initialRPM = 0.0;
    UA_Variant_setScalar(&rpmAttr.value, &initialRPM, &UA_TYPES[UA_TYPES_DOUBLE]);
    
    UA_NodeId rpmId = UA_NODEID_NUMERIC(nsIdx, 3);
    UA_QualifiedName rpmName = UA_QUALIFIEDNAME_ALLOC(nsIdx, "FlywheelRPM");
    
    status = UA_Server_addVariableNode(
        server, rpmId,
        parentNodeId,
        parentReferenceNodeId,
        rpmName,
        typeDefinition,
        rpmAttr, NULL, NULL);
    
    UA_String_clear(&rpmAttr.displayName.text);
    UA_String_clear(&rpmAttr.description.text);
    UA_QualifiedName_clear(&rpmName);
    
    if (status != UA_STATUSCODE_GOOD) {
        std::cerr << "Error creating flywheel RPM node: " << UA_StatusCode_name(status) << std::endl;
        UA_Server_delete(server);
        return 1;
    }
    
    std::cout << "Server running on opc.tcp://localhost:4840" << std::endl;
    std::cout << "Variables available:" << std::endl;
    std::cout << "  - Voltage (ID: ns=1;i=1)" << std::endl;
    std::cout << "  - Current (ID: ns=1;i=2)" << std::endl;
    std::cout << "  - Flywheel RPM (ID: ns=1;i=3)" << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;
    
    // Запускаем сервер
    status = UA_Server_run_startup(server);
    if (status != UA_STATUSCODE_GOOD) {
        std::cerr << "Failed to start server: " << UA_StatusCode_name(status) << std::endl;
        UA_Server_delete(server);
        return 1;
    }
    
    // Инициализация генераторов случайных чисел
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<double> voltageDist(190.0, 240.0);
    std::uniform_real_distribution<double> currentDist(0.5, 15.0);
    
    // Основной цикл
    while (running) {
        double voltage = voltageDist(gen);
        double current = currentDist(gen);
        
        std::cout << "\rVoltage: " << voltage << " V, Current: " << current << " A" 
                  << " (Flywheel RPM is static and can be modified by client)" << std::flush;
        
        // Обновляем значения Voltage и Current (динамические переменные)
        UA_Variant voltageVar;
        UA_Variant_init(&voltageVar);
        UA_Variant_setScalarCopy(&voltageVar, &voltage, &UA_TYPES[UA_TYPES_DOUBLE]);
        UA_Server_writeValue(server, voltageId, voltageVar);
        UA_Variant_clear(&voltageVar);
        
        UA_Variant currentVar;
        UA_Variant_init(&currentVar);
        UA_Variant_setScalarCopy(&currentVar, &current, &UA_TYPES[UA_TYPES_DOUBLE]);
        UA_Server_writeValue(server, currentId, currentVar);
        UA_Variant_clear(&currentVar);
        
        // ОБОРОТЫ МАХОВИКА НЕ ОБНОВЛЯЕМ - это статичная переменная, которую может менять только клиент
        
        // Обработка сетевых событий
        UA_Server_run_iterate(server, false);
        
        // Пауза
        std::this_thread::sleep_for(std::chrono::milliseconds(330));
    }
    
    // Остановка
    std::cout << "\nStopping server..." << std::endl;
    UA_Server_run_shutdown(server);
    UA_Server_delete(server);
    
    std::cout << "Server stopped." << std::endl;
    return 0;
}