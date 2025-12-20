#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <iostream>
#include <random>
#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

class UAString {
private:
    UA_String str;
    bool ownsMemory;
    
public:
    UAString(const char* cstr) : ownsMemory(true) {
        str.length = strlen(cstr);
        str.data = (UA_Byte*)UA_malloc(str.length);
        if (str.data) {
            memcpy(str.data, cstr, str.length);
        } else {
            str.length = 0;
            ownsMemory = false;
        }
    }
    
    UAString(const UAString&) = delete;
    UAString& operator=(const UAString&) = delete;
    
    UAString(UAString&& other) noexcept : str(other.str), ownsMemory(other.ownsMemory) {
        other.ownsMemory = false;
        other.str.data = nullptr;
        other.str.length = 0;
    }
    
    ~UAString() {
        if (ownsMemory) {
            UA_String_clear(&str);
        }
    }
    
    UA_String* get() { return &str; }
    const UA_String* get() const { return &str; }
};

class UALocalizedText {
private:
    UA_LocalizedText text;
    
public:
    UALocalizedText(const char* locale, const char* txt) {
        text.locale = UA_STRING_ALLOC(locale);
        text.text = UA_STRING_ALLOC(txt);
    }
    
    ~UALocalizedText() {
        UA_LocalizedText_clear(&text);
    }
    
    UALocalizedText(const UALocalizedText&) = delete;
    UALocalizedText& operator=(const UALocalizedText&) = delete;
    
    UALocalizedText(UALocalizedText&& other) noexcept : text(other.text) {
        other.text.locale.data = nullptr;
        other.text.locale.length = 0;
        other.text.text.data = nullptr;
        other.text.text.length = 0;
    }
    
    UA_LocalizedText* get() { return &text; }
    const UA_LocalizedText* get() const { return &text; }
};

class UAQualifiedName {
private:
    UA_QualifiedName name;
    
public:
    UAQualifiedName(UA_UInt16 nsIndex, const char* nameStr) {
        name.namespaceIndex = nsIndex;
        name.name = UA_STRING_ALLOC(nameStr);
    }
    
    ~UAQualifiedName() {
        UA_QualifiedName_clear(&name);
    }
    
    UAQualifiedName(const UAQualifiedName&) = delete;
    UAQualifiedName& operator=(const UAQualifiedName&) = delete;
    
    UAQualifiedName(UAQualifiedName&& other) noexcept : name(other.name) {
        other.name.name.data = nullptr;
        other.name.name.length = 0;
    }
    
    UA_QualifiedName* get() { return &name; }
    const UA_QualifiedName* get() const { return &name; }
};

class OPCUANode {
protected:
    UA_Server* server;
    UA_NodeId nodeId;
    
public:
    OPCUANode(UA_Server* srv, const UA_NodeId& id) 
        : server(srv), nodeId(id) {}
    
    virtual ~OPCUANode() = default;
    
    OPCUANode(const OPCUANode&) = delete;
    OPCUANode& operator=(const OPCUANode&) = delete;
    
    UA_NodeId getNodeId() const { return nodeId; }
    UA_Server* getServer() const { return server; }
    
    virtual void initialize() = 0;
};

class OPCUAVariable : public OPCUANode {
protected:
    std::string displayName;
    std::string description;
    std::string browseName;
    double initialValue;
    
public:
    OPCUAVariable(UA_Server* srv, UA_UInt16 nsIndex, UA_UInt32 id, 
                  const std::string& browseName, const std::string& displayName,
                  const std::string& description, double initialValue)
        : OPCUANode(srv, UA_NODEID_NUMERIC(nsIndex, id)),
          displayName(displayName), 
          description(description),
          browseName(browseName),
          initialValue(initialValue) {}
    
    virtual void initialize() override {
        UA_VariableAttributes attr = UA_VariableAttributes_default;
        
        UALocalizedText displayNameText("en-US", displayName.c_str());
        UALocalizedText descriptionText("en-US", description.c_str());
        UAQualifiedName qualifiedName(nodeId.namespaceIndex, browseName.c_str());
        
        attr.displayName = *displayNameText.get();
        attr.description = *descriptionText.get();
        attr.dataType = UA_TYPES[UA_TYPES_DOUBLE].typeId;
        attr.valueRank = UA_VALUERANK_SCALAR;
        attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
        attr.userAccessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
        
        UA_Variant_setScalarCopy(&attr.value, &initialValue, &UA_TYPES[UA_TYPES_DOUBLE]);
        
        UA_StatusCode status = UA_Server_addVariableNode(server, nodeId,
            UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
            UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
            *qualifiedName.get(),
            UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
            attr, NULL, NULL);
        
        UA_Variant_clear(&attr.value);
    }
    
    void writeValue(double value) {
        UA_Variant var;
        UA_Variant_init(&var);
        UA_Variant_setScalarCopy(&var, &value, &UA_TYPES[UA_TYPES_DOUBLE]);
        UA_StatusCode status = UA_Server_writeValue(server, nodeId, var);
        if (status != UA_STATUSCODE_GOOD) {
            std::cerr << "Failed to write value to node: " << UA_StatusCode_name(status) << std::endl;
        }
        UA_Variant_clear(&var);
    }
    
    double readValue() {
    UA_Variant var;
    UA_Variant_init(&var);
    UA_StatusCode status = UA_Server_readValue(server, nodeId, &var);
    double value = 0.0;
    if (status == UA_STATUSCODE_GOOD && var.type == &UA_TYPES[UA_TYPES_DOUBLE] && var.data) {
        value = *static_cast<double*>(var.data);
    }
    UA_Variant_clear(&var);
    return value;
}
};

class OPCUAComponentVariable : public OPCUAVariable {
private:
    UA_NodeId parentNodeId;
    
public:
    OPCUAComponentVariable(UA_Server* srv, UA_UInt16 nsIndex, UA_UInt32 id,
                          const std::string& browseName, const std::string& displayName,
                          const std::string& description, double initialValue,
                          const UA_NodeId& parentId)
        : OPCUAVariable(srv, nsIndex, id, browseName, displayName, description, initialValue),
          parentNodeId(parentId) {}
    
    void initialize() override {
        UA_VariableAttributes attr = UA_VariableAttributes_default;
        
        UALocalizedText displayNameText("en-US", displayName.c_str());
        UALocalizedText descriptionText("en-US", description.c_str());
        UAQualifiedName qualifiedName(nodeId.namespaceIndex, browseName.c_str());
        
        attr.displayName = *displayNameText.get();
        attr.description = *descriptionText.get();
        attr.dataType = UA_TYPES[UA_TYPES_DOUBLE].typeId;
        attr.valueRank = UA_VALUERANK_SCALAR;
        attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
        attr.userAccessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
        
        UA_Variant_setScalarCopy(&attr.value, &initialValue, &UA_TYPES[UA_TYPES_DOUBLE]);
        
        UA_StatusCode status = UA_Server_addVariableNode(server, nodeId,
            parentNodeId,
            UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
            *qualifiedName.get(),
            UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
            attr, NULL, NULL);
        
        UA_Variant_clear(&attr.value);
    }
};

class OPCUADevice : public OPCUANode {
protected:
    std::string displayName;
    std::string description;
    std::string browseName;
    std::vector<std::unique_ptr<OPCUAComponentVariable>> components;
    
public:
    OPCUADevice(UA_Server* srv, UA_UInt16 nsIndex, UA_UInt32 id,
                const std::string& browseName, const std::string& displayName,
                const std::string& description)
        : OPCUANode(srv, UA_NODEID_NUMERIC(nsIndex, id)),
          displayName(displayName),
          description(description),
          browseName(browseName) {}
    
    void initialize() override {
        UA_ObjectAttributes attr = UA_ObjectAttributes_default;
        
        UALocalizedText displayNameText("en-US", displayName.c_str());
        UALocalizedText descriptionText("en-US", description.c_str());
        UAQualifiedName qualifiedName(nodeId.namespaceIndex, browseName.c_str());
        
        attr.displayName = *displayNameText.get();
        attr.description = *descriptionText.get();
        
        UA_StatusCode status = UA_Server_addObjectNode(
            server, nodeId,
            UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
            UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
            *qualifiedName.get(),
            UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE),
            attr, NULL, NULL);
        
        if (status != UA_STATUSCODE_GOOD) {
            std::cerr << "Failed to add device node: " << UA_StatusCode_name(status) << std::endl;
            return;
        }
        
        for (auto& component : components) {
            if (component) {
                component->initialize();
            }
        }
    }
    
    void addComponent(std::unique_ptr<OPCUAComponentVariable> component) {
        components.push_back(std::move(component));
    }
    
    virtual void updateValues() = 0;
    
    const UA_NodeId& getParentNodeId() const { return nodeId; }
    std::string getBrowseName() const { return browseName; }
    std::string getDisplayName() const { return displayName; }
};


double smoothStep(std::mt19937& rng, double current, double minVal, double maxVal, double maxStep) {
    std::uniform_real_distribution<double> dist(-maxStep, maxStep);
    double next = current + dist(rng);
    return std::clamp(next, minVal, maxVal);
}

class Multimeter : public OPCUADevice {
private:
    OPCUAComponentVariable* voltage;
    OPCUAComponentVariable* current;
    OPCUAComponentVariable* resistance;
    OPCUAComponentVariable* power;
    std::mt19937 rng;

public:
    Multimeter(UA_Server* srv, UA_UInt16 nsIndex)
        : OPCUADevice(srv, nsIndex, 100, "Multimeter", "Мультиметр", "Электрический измерительный прибор"),
          rng(std::random_device{}()),
          voltage(nullptr), current(nullptr), resistance(nullptr), power(nullptr)
    {
        auto voltageVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 101, "Voltage", "Напряжение", "Измеренное напряжение (Вольты)", 220.0, nodeId);
        auto currentVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 102, "Current", "Сила тока", "Измеренная сила тока (Амперы)", 5.0, nodeId);
        auto resistanceVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 103, "Resistance", "Сопротивление", "Измеренное сопротивление (Омы)", 44.0, nodeId);
        auto powerVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 104, "Power", "Мощность", "Расчетная мощность (Ватты)", 1100.0, nodeId);

        voltage = voltageVar.get();
        current = currentVar.get();
        resistance = resistanceVar.get();
        power = powerVar.get();

        addComponent(std::move(voltageVar));
        addComponent(std::move(currentVar));
        addComponent(std::move(resistanceVar));
        addComponent(std::move(powerVar));
    }

    void updateValues() override {
        double v = smoothStep(rng, voltage->readValue(), 190.0, 240.0, 5.0);
        double c = smoothStep(rng, current->readValue(), 0.5, 15.0, 0.5);
        double r = (c > 0.1) ? v / c : 100.0;
        double p = v * c;

        voltage->writeValue(v);
        current->writeValue(c);
        resistance->writeValue(r);
        power->writeValue(p);

        std::cout << "Мультиметр: Напряжение = " << v << " В, Ток = " << c
                << " А, Сопротивление = " << r << " Ом, Мощность = " << p << " Вт" << std::endl;
    }


    void printStatus() const {
        std::cout << "Мультиметр создан с узлами:" << std::endl;
        std::cout << " - Напряжение (ns=1;i=101)" << std::endl;
        std::cout << " - Сила тока (ns=1;i=102)" << std::endl;
        std::cout << " - Сопротивление (ns=1;i=103)" << std::endl;
        std::cout << " - Мощность (ns=1;i=104)" << std::endl;
    }
};

class Machine : public OPCUADevice {
private:
    OPCUAComponentVariable* flywheelRPM;
    OPCUAComponentVariable* power;
    OPCUAComponentVariable* voltage;
    OPCUAComponentVariable* energyConsumption;
    OPCUAComponentVariable* targetRPM;
    OPCUAComponentVariable* rpmControlMode;

    std::mt19937 rng;
    double baseRPM;
    double currentRPM;
    bool manualControl;
    double lastTargetRPM;
    double lastControlMode;

    void checkAndUpdateVariables() {
        if (targetRPM) {
            double currentTarget = targetRPM->readValue();
            if (fabs(currentTarget - lastTargetRPM) > 0.1) {
                lastTargetRPM = currentTarget;
                currentTarget = std::clamp(currentTarget, 0.0, 3000.0);
                baseRPM = currentTarget;
                manualControl = true;
                if (rpmControlMode) {
                    rpmControlMode->writeValue(1.0);
                    lastControlMode = 1.0;
                }
                std::cout << "Обнаружены новые целевые обороты: " << currentTarget << " об/мин" << std::endl;
            }
        }
        if (rpmControlMode) {
            double currentMode = rpmControlMode->readValue();
            if (fabs(currentMode - lastControlMode) > 0.1) {
                lastControlMode = currentMode;
                manualControl = (currentMode == 1.0);
                std::cout << "Обнаружено изменение режима: " << (manualControl ? "РУЧНОЙ" : "АВТО") << std::endl;
            }
        }
    }

public:
    Machine(UA_Server* srv, UA_UInt16 nsIndex)
        : OPCUADevice(srv, nsIndex, 200, "Machine", "Станок", "Промышленный станок с электроприводом"),
          rng(std::random_device{}()),
          baseRPM(1500.0), currentRPM(1500.0), manualControl(false),
          lastTargetRPM(1500.0), lastControlMode(0.0),
          flywheelRPM(nullptr), power(nullptr), voltage(nullptr), energyConsumption(nullptr),
          targetRPM(nullptr), rpmControlMode(nullptr)
    {
        auto flywheelRPMVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 201, "FlywheelRPM", "Обороты маховика", "Скорость вращения маховика (об/мин)", baseRPM, nodeId);
        auto powerVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 202, "Power", "Мощность", "Потребляемая мощность (кВт)", 7.5, nodeId);
        auto voltageVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 203, "Voltage", "Напряжение", "Рабочее напряжение (Вольты)", 380.0, nodeId);
        auto energyVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 204, "EnergyConsumption", "Потребление энергии", "Потребление энергии (кВт·ч)", 56.3, nodeId);
        auto targetRPMVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 205, "TargetRPM", "Целевые обороты", "Заданные клиентом обороты (об/мин)", baseRPM, nodeId);
        auto controlModeVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 206, "RPMControlMode", "Режим управления", "0=авто, 1=ручной", 0.0, nodeId);

        flywheelRPM = flywheelRPMVar.get();
        power = powerVar.get();
        voltage = voltageVar.get();
        energyConsumption = energyVar.get();
        targetRPM = targetRPMVar.get();
        rpmControlMode = controlModeVar.get();

        addComponent(std::move(flywheelRPMVar));
        addComponent(std::move(powerVar));
        addComponent(std::move(voltageVar));
        addComponent(std::move(energyVar));
        addComponent(std::move(targetRPMVar));
        addComponent(std::move(controlModeVar));
    }

    void updateValues() override {
    checkAndUpdateVariables();

    double rpm;
    if (manualControl) {
        double delta = baseRPM - currentRPM;
        double step = std::clamp(delta * 0.1, -50.0, 50.0);
        currentRPM += step;
        if (fabs(baseRPM - currentRPM) < 1.0)
            currentRPM = baseRPM;
        rpm = currentRPM;
    } else {
        std::normal_distribution<double> rpmNoise(0.0, 10.0);
        rpm = std::max(0.0, baseRPM + rpmNoise(rng));
        currentRPM = rpm;
    }

    double pwrMin = 0.0;
    double pwrMax = 15.0;
    double pwrMaxStep = 0.2;
    double pwr = smoothStep(rng, power->readValue(), pwrMin, pwrMax, pwrMaxStep);

    double voltMin = 370.0;
    double voltMax = 390.0;
    double voltMaxStep = 2.0;
    double volt = smoothStep(rng, voltage->readValue(), voltMin, voltMax, voltMaxStep);

    double energyMin = 50.0;
    double energyMax = 60.0;
    double energyMaxStep = 0.1;
    double energy = smoothStep(rng, energyConsumption->readValue(), energyMin, energyMax, energyMaxStep);

    if (flywheelRPM) flywheelRPM->writeValue(rpm);
    if (power) power->writeValue(pwr);
    if (voltage) voltage->writeValue(volt);
    if (energyConsumption) energyConsumption->writeValue(energy);

    std::string modeStr = manualControl ? "РУЧНОЙ" : "АВТО";
    std::cout << "Станок (" << modeStr << "): Обороты = " << rpm
              << " об/мин, Мощность = " << pwr
              << " кВт, Напряжение = " << volt
              << " В, Энергия = " << energy << " кВт·ч" << std::endl;
}


    void printStatus() const {
        std::cout << "Станок создан с узлами:" << std::endl;
        std::cout << " - Обороты маховика (ns=1;i=201)" << std::endl;
        std::cout << " - Мощность (ns=1;i=202)" << std::endl;
        std::cout << " - Напряжение (ns=1;i=203)" << std::endl;
        std::cout << " - Потребление энергии (ns=1;i=204)" << std::endl;
        std::cout << " - Целевые обороты (ns=1;i=205) - ДЛЯ ЗАПИСИ" << std::endl;
        std::cout << " - Режим управления (ns=1;i=206) - ДЛЯ ЗАПИСИ (0=авто,1=ручной)" << std::endl;
    }
};

class Computer : public OPCUADevice {
private:
    OPCUAComponentVariable* fan1;
    OPCUAComponentVariable* fan2;
    OPCUAComponentVariable* fan3;
    OPCUAComponentVariable* cpuLoad;
    OPCUAComponentVariable* gpuLoad;
    OPCUAComponentVariable* ramUsage;

    std::mt19937 rng;

public:
    Computer(UA_Server* srv, UA_UInt16 nsIndex)
        : OPCUADevice(srv, nsIndex, 300, "Computer", "Компьютер", "Системный блок с мониторингом параметров"),
          rng(std::random_device{}()),
          fan1(nullptr), fan2(nullptr), fan3(nullptr),
          cpuLoad(nullptr), gpuLoad(nullptr), ramUsage(nullptr)
    {
        auto fan1Var = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 301, "Fan1", "Вентилятор 1", "Скорость вентилятора ЦП (об/мин)", 1200.0, nodeId);
        auto fan2Var = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 302, "Fan2", "Вентилятор 2", "Скорость вентилятора корпуса (об/мин)", 800.0, nodeId);
        auto fan3Var = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 303, "Fan3", "Вентилятор 3", "Скорость вентилятора блока питания (об/мин)", 1000.0, nodeId);
        auto cpuLoadVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 304, "CPULoad", "Загрузка ЦП", "Загрузка центрального процессора (%)", 30.0, nodeId);
        auto gpuLoadVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 305, "GPULoad", "Загрузка ГП", "Загрузка графического процессора (%)", 25.0, nodeId);
        auto ramUsageVar = std::make_unique<OPCUAComponentVariable>(
            srv, nsIndex, 306, "RAMUsage", "Использование ОЗУ", "Использование оперативной памяти (%)", 45.0, nodeId);

        fan1 = fan1Var.get();
        fan2 = fan2Var.get();
        fan3 = fan3Var.get();
        cpuLoad = cpuLoadVar.get();
        gpuLoad = gpuLoadVar.get();
        ramUsage = ramUsageVar.get();

        addComponent(std::move(fan1Var));
        addComponent(std::move(fan2Var));
        addComponent(std::move(fan3Var));
        addComponent(std::move(cpuLoadVar));
        addComponent(std::move(gpuLoadVar));
        addComponent(std::move(ramUsageVar));
    }

    void updateValues() override {
    double cpu = smoothStep(rng, cpuLoad->readValue(), 20.0, 80.0, 2.0);
    double gpu = smoothStep(rng, gpuLoad->readValue(), 20.0, 80.0, 2.0);
    double ram = smoothStep(rng, ramUsage->readValue(), 30.0, 70.0, 1.5);

    double f1 = smoothStep(rng, fan1->readValue(), 800.0, 1800.0, 50.0);
    double f2 = smoothStep(rng, fan2->readValue(), 800.0, 1800.0, 50.0);
    double f3 = smoothStep(rng, fan3->readValue(), 800.0, 1800.0, 50.0);

    cpuLoad->writeValue(cpu);
    gpuLoad->writeValue(gpu);
    ramUsage->writeValue(ram);
    fan1->writeValue(f1);
    fan2->writeValue(f2);
    fan3->writeValue(f3);

    std::cout << "Компьютер: Вентиляторы = [" << f1 << ", " << f2 << ", " << f3
              << "], ЦП = " << cpu << "%, ГП = " << gpu << "%, ОЗУ = " << ram << "%" << std::endl;
}


    void printStatus() const {
        std::cout << "Компьютер создан с узлами:" << std::endl;
        std::cout << " - Вентилятор 1 (ns=1;i=301)" << std::endl;
        std::cout << " - Вентилятор 2 (ns=1;i=302)" << std::endl;
        std::cout << " - Вентилятор 3 (ns=1;i=303)" << std::endl;
        std::cout << " - Загрузка ЦП (ns=1;i=304)" << std::endl;
        std::cout << " - Загрузка ГП (ns=1;i=305)" << std::endl;
        std::cout << " - Использование ОЗУ (ns=1;i=306)" << std::endl;
    }
};

class OPCUAServer {
private:
    UA_Server* server;
    UA_UInt16 namespaceIndex;
    std::atomic<bool> running;
    std::unique_ptr<Multimeter> multimeter;
    std::unique_ptr<Machine> machine;
    std::unique_ptr<Computer> computer;
    
public:
    OPCUAServer() : server(nullptr), namespaceIndex(0), running(true) {
        initConsole();
    }
    
    ~OPCUAServer() {
        stop();
    }
    
    OPCUAServer(const OPCUAServer&) = delete;
    OPCUAServer& operator=(const OPCUAServer&) = delete;
    
    bool initialize() {
        std::cout << "OPC UA Server initializing..." << std::endl;
        
        server = UA_Server_new();
        if (!server) {
            std::cerr << "Failed to create server" << std::endl;
            return false;
        }
        
        UA_ServerConfig* config = UA_Server_getConfig(server);
        UA_ServerConfig_setDefault(config);
        
        namespaceIndex = UA_Server_addNamespace(server, "EquipmentNamespace");
        
        multimeter = std::make_unique<Multimeter>(server, namespaceIndex);
        multimeter->initialize();
        
        machine = std::make_unique<Machine>(server, namespaceIndex);
        machine->initialize();
        
        computer = std::make_unique<Computer>(server, namespaceIndex);
        computer->initialize();
        
        return true;
    }
    
    bool start() {
        std::cout << "\n===========================================" << std::endl;
        std::cout << "OPC UA Server запущен на opc.tcp://localhost:4840" << std::endl;
        std::cout << "===========================================" << std::endl;
        std::cout << "\nСтруктура устройств и переменных:" << std::endl;
        std::cout << "\n1. Мультиметр (ID: ns=" << namespaceIndex << ";i=100)" << std::endl;
        std::cout << "   ├── Напряжение (ID: ns=" << namespaceIndex << ";i=101)" << std::endl;
        std::cout << "   ├── Сила тока (ID: ns=" << namespaceIndex << ";i=102)" << std::endl;
        std::cout << "   ├── Сопротивление (ID: ns=" << namespaceIndex << ";i=103)" << std::endl;
        std::cout << "   └── Мощность (ID: ns=" << namespaceIndex << ";i=104)" << std::endl;
        
        std::cout << "\n2. Станок (ID: ns=" << namespaceIndex << ";i=200)" << std::endl;
        std::cout << "   ├── Обороты маховика (ID: ns=" << namespaceIndex << ";i=201)" << std::endl;
        std::cout << "   ├── Мощность (ID: ns=" << namespaceIndex << ";i=202)" << std::endl;
        std::cout << "   ├── Напряжение (ID: ns=" << namespaceIndex << ";i=203)" << std::endl;
        std::cout << "   ├── Потребление энергии (ID: ns=" << namespaceIndex << ";i=204)" << std::endl;
        std::cout << "   ├── Целевые обороты (ID: ns=" << namespaceIndex << ";i=205) - ДЛЯ ЗАПИСИ" << std::endl;
        std::cout << "   └── Режим управления (ID: ns=" << namespaceIndex << ";i=206) - ДЛЯ ЗАПИСИ" << std::endl;
        std::cout << "       (0=автоматический, 1=ручной)" << std::endl;
        
        std::cout << "\n3. Компьютер (ID: ns=" << namespaceIndex << ";i=300)" << std::endl;
        std::cout << "   ├── Вентилятор 1 (ID: ns=" << namespaceIndex << ";i=301)" << std::endl;
        std::cout << "   ├── Вентилятор 2 (ID: ns=" << namespaceIndex << ";i=302)" << std::endl;
        std::cout << "   ├── Вентилятор 3 (ID: ns=" << namespaceIndex << ";i=303)" << std::endl;
        std::cout << "   ├── Загрузка ЦП (ID: ns=" << namespaceIndex << ";i=304)" << std::endl;
        std::cout << "   ├── Загрузка ГП (ID: ns=" << namespaceIndex << ";i=305)" << std::endl;
        std::cout << "   └── Использование ОЗУ (ID: ns=" << namespaceIndex << ";i=306)" << std::endl;
        
        std::cout << "\n===========================================" << std::endl;
        std::cout << "Инструкция по управлению станциком:" << std::endl;
        std::cout << "1. Для ручного управления записать в TargetRPM значение от 0 до 3000" << std::endl;
        std::cout << "2. Для переключения режима записать в RPMControlMode: 0=авто, 1=ручной" << std::endl;
        std::cout << "3. Обороты плавно изменятся до заданного значения" << std::endl;
        std::cout << "===========================================" << std::endl;
        std::cout << "Для остановки сервера нажмите Ctrl+C" << std::endl;
        std::cout << "===========================================\n" << std::endl;
        
        UA_StatusCode status = UA_Server_run_startup(server);
        if (status != UA_STATUSCODE_GOOD) {
            std::cerr << "Failed to start server: " << UA_StatusCode_name(status) << std::endl;
            return false;
        }
        
        return true;
    }
    
    void run() {
        int counter = 0;
        while (running) {
            clearConsole();
            
            std::cout << "===========================================" << std::endl;
            std::cout << "ЦИКЛ ОБНОВЛЕНИЯ: " << ++counter << std::endl;
            std::cout << "===========================================" << std::endl;
            
            if (multimeter) {
                multimeter->updateValues();
            }
            
            if (machine) {
                machine->updateValues();
            }
            
            if (computer) {
                computer->updateValues();
            }
            
            std::cout << "===========================================" << std::endl;
            std::cout << "Для управления станциком используйте OPC UA клиент" << std::endl;
            std::cout << "===========================================" << std::endl;
            
            UA_Server_run_iterate(server, false);
            
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    void stop() {
        if (!running) return;
        
        running = false;
        
        if (server) {
            std::cout << "\nОстановка сервера..." << std::endl;
            
            computer.reset();
            machine.reset();
            multimeter.reset();
            
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
            UA_StatusCode status = UA_Server_run_shutdown(server);
            if (status != UA_STATUSCODE_GOOD) {
                std::cerr << "Ошибка при остановке сервера: " << UA_StatusCode_name(status) << std::endl;
            }
            
            UA_Server_delete(server);
            server = nullptr;
            
            std::cout << "Сервер остановлен." << std::endl;
        }
    }
    
    bool isRunning() const { return running; }
    
private:
    void initConsole() {
#ifdef _WIN32
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
#endif
    }
    
    void clearConsole() {
#ifdef _WIN32
        system("cls");
#else
        system("clear");
#endif
    }
};

std::atomic<bool> globalRunning(true);
OPCUAServer* g_serverInstance = nullptr;

void signalHandler(int signal) {
    (void)signal;
    std::cout << "\nПолучен сигнал остановки, останавливаю сервер..." << std::endl;
    globalRunning = false;
    
    if (g_serverInstance) {
        g_serverInstance->stop();
    }
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    
    std::cout << "===========================================" << std::endl;
    std::cout << "Запуск OPC UA сервера..." << std::endl;
    std::cout << "Версия: 1.0" << std::endl;
    std::cout << "===========================================\n" << std::endl;
    
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    
#ifdef _WIN32
    SetConsoleCtrlHandler([](DWORD ctrlType) -> BOOL {
        switch (ctrlType) {
            case CTRL_C_EVENT:
                std::cout << "\nПолучен Ctrl+C. Завершение работы..." << std::endl;
                break;
            case CTRL_BREAK_EVENT:
                std::cout << "\nПолучен Ctrl+Break. Завершение работы..." << std::endl;
                break;
            case CTRL_CLOSE_EVENT:
                std::cout << "\nЗакрытие консоли. Завершение работы..." << std::endl;
                break;
            default:
                break;
        }
        signalHandler(0);
        return TRUE;
    }, TRUE);
#endif
    
    try {
        OPCUAServer server;
        g_serverInstance = &server;
        
        if (!server.initialize()) {
            std::cerr << "Ошибка инициализации сервера!" << std::endl;
            return 1;
        }
        
        if (!server.start()) {
            std::cerr << "Ошибка запуска сервера!" << std::endl;
            return 1;
        }
        
        std::thread serverThread([&server]() {
            server.run();
        });
        
        while (globalRunning && server.isRunning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        
        server.stop();
        
        if (serverThread.joinable()) {
            serverThread.join();
        }
        
        std::cout << "\n===========================================" << std::endl;
        std::cout << "Сервер успешно завершил работу." << std::endl;
        std::cout << "===========================================" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "\nИсключение: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\nНеизвестное исключение" << std::endl;
        return 1;
    }
    
    g_serverInstance = nullptr;
    
    return 0;
}
