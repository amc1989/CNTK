//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#pragma once

#include "stdafx.h"
#include "CNTKLibrary.h"
#include "PrimitiveFunction.h"
#include "ComputationNetwork.h"
#include "BackCompat.h"

namespace CNTK
{
    class CNTKBackPropState final : public BackPropState
    {
    public:
        CNTKBackPropState(const FunctionPtr& function, const DeviceDescriptor& computeDevice, const std::unordered_map<Variable, uint64_t>& backpropRootsForwardTimeStamps)
            : BackPropState(function, computeDevice), m_backpropRootsForwardTimeStamps(backpropRootsForwardTimeStamps)
        {}

        const std::unordered_map<Variable, uint64_t>& BackpropRootsForwardTimeStamps() const
        {
            return m_backpropRootsForwardTimeStamps; 
        }

    private:
        std::unordered_map<Variable, uint64_t> m_backpropRootsForwardTimeStamps;
    };
    typedef std::shared_ptr<CNTKBackPropState> CNTKBackPropStatePtr;

    class CompositeFunction;
    typedef std::shared_ptr<CompositeFunction> CompositeFunctionPtr;

    class CompositeFunction final : public Function
    {
        friend class Function;
        friend class Trainer;
        friend class CompositeMinibatchSource;
        friend class PackedValue;

        template <typename T, typename ...CtorArgTypes>
        friend inline std::shared_ptr<T> MakeSharedObject(CtorArgTypes&& ...ctorArgs);

        friend void Internal::SaveAsLegacyModel(const FunctionPtr& rootFunction, const std::wstring& modelFile);

        friend void ComputeInputPerDimMeansAndInvStdDevs(const MinibatchSourcePtr& minibatchSource,
                                                         std::unordered_map<StreamInformation, std::pair<NDArrayViewPtr, NDArrayViewPtr>>& computedMeanAndInvStdDevs,
                                                         const DeviceDescriptor& device /*= DeviceDescriptor::CPUDevice()*/);

        static std::atomic<unsigned int> s_nextAutoGeneratedDynamicAxis;

        static const std::wstring CompositeFunctionOpName;

    public:
        static const std::wstring InternalDefaultDynamicAxisName;
        static const std::wstring InternalNoSequenceAxisName;

        static Axis NextAutoGeneratedDynamicAxis()
        {
            static const std::wstring s_autoGeneratedDynamicAxisNamePrefix = L"autoGeneratedDynamicAxis_";
            return Axis(s_autoGeneratedDynamicAxisNamePrefix + std::to_wstring(s_nextAutoGeneratedDynamicAxis++));
        }

    public:
        static CompositeFunctionPtr Create(const FunctionPtr& rootFunction, const std::wstring& name = L"", const std::wstring& uid = L"")
        {
            std::unordered_set<FunctionPtr> visitedFunctions;

            // Call Collect to get the set of all functions in the graph
            Collect(rootFunction, visitedFunctions);

            return MakeSharedObject<CompositeFunction>(rootFunction, std::move(visitedFunctions), name, uid);
        }

        virtual BackPropStatePtr Forward(const std::unordered_map<Variable, ValuePtr>& arguments,
                                         std::unordered_map<Variable, ValuePtr>& outputs,
                                         const DeviceDescriptor& computeDevice,
                                         const std::unordered_set<Variable>& outputsToRetainBackwardStateFor) override;

        virtual void Backward(const BackPropStatePtr& state,
                              const std::unordered_map<Variable, ValuePtr>& rootGradientValues,
                              std::unordered_map<Variable, ValuePtr>& backPropagatedGradientValuesForInputs) override;

        Dictionary SerializeBlockComposite() const;

        virtual Dictionary Serialize() const override;
        
        virtual size_t CurrentVersion() const override { return s_serializationVersion; }

        static FunctionPtr DeserializeBlockComposite(const Dictionary& dict,
                                                     const std::unordered_set<FunctionPtr>& allPrimitiveFunctions,
                                                     const std::unordered_map<Variable, Variable>& allPlaceholderReplacements,
                                                     const CNTK::DeviceDescriptor& device);

        static FunctionPtr Deserialize(const Dictionary& dictionary, const CNTK::DeviceDescriptor& device);

        virtual const std::wstring& OpName() const override
        {
            return CompositeFunctionOpName;
        }

        template <typename FunctionType>
        static void Traverse(const FunctionPtr& rootFunction, const FunctionType& functor)
        {
            std::unordered_set<FunctionPtr> visitedFunctions;
            Traverse(rootFunction, visitedFunctions, functor);
        }

        // Recursively traverses the Function graph underlying the 'rootFunction' invoking the provided functor for all visited nodes in the graph.
        template <typename FunctionType>
        static void Traverse(const FunctionPtr& rootFunction, std::unordered_set<FunctionPtr>& visitedFunctions, const FunctionType& functor)
        {
            visitedFunctions.insert(rootFunction);
            functor(rootFunction);

            std::vector<Variable> rootFunctionInputs = rootFunction->Inputs();
            for (const auto& rootInput : rootFunctionInputs)
            {
                if (rootInput.IsOutput() && visitedFunctions.find(rootInput.Owner()) == visitedFunctions.end())
                {
                    const auto& function = rootInput.Owner();
                    Traverse(function, visitedFunctions, functor);
                }
            }
        }

    private:
        // Replace any PlaceHolder Variables in the graph of Functions underlying 'this' CompositeFunction. All PlaceHolder variables
        // should have been replaced before performing any Forward compute of 'this' Function.
        virtual void OnPlaceholdersReplaced(const std::unordered_map<Variable, Variable>& placeholderReplacements,
                                            std::unordered_set<Variable>& replacedPlaceholders) override
        {
            // If any of the placeholders were replaced with Output variables, let's add the graph of function underneath 
            // each of those to 'm_allPrimitiveFunctions' set
            for (auto replacedPlaceholder : replacedPlaceholders)
            {
                auto replacingVariable = placeholderReplacements.at(replacedPlaceholder);
                if (replacingVariable.IsOutput())
                {
                    auto ownerFunc = replacingVariable.Owner();
                    std::unordered_set<FunctionPtr> visitedFunctions2;
                    Collect(ownerFunc, visitedFunctions2);

                    // Add the newly visited functions to 'm_allPrimitiveFunctions' set
                    m_allPrimitiveFunctions.insert(visitedFunctions2.begin(), visitedFunctions2.end());
                }
            }
        }

        CompositeFunction(const FunctionPtr& rootFunction, std::unordered_set<FunctionPtr>&& allPrimitiveFunctions, const std::wstring& name, const std::wstring& uid = Internal::GenerateUid(L"CompositeFunction"))
            : Function({}, rootFunction->Outputs(), Dictionary(), rootFunction, name, uid),
            m_allPrimitiveFunctions(std::move(allPrimitiveFunctions)), m_networkMatricesAllocated(false)
        {}

        std::vector<Variable> DetermineInputs() const
        {
            const auto& root = RootFunction();
            std::unordered_set<FunctionPtr> visitedFunctions;
            return DetermineInputs(root, visitedFunctions);
        }

         // Recursively traverses the Function graph and populates the provided set of functions.
        static void Collect(const FunctionPtr& rootFunction, std::unordered_set<FunctionPtr>& functions)
        {
            // Call Traverse to get the set of all functions in the graph
            Traverse(rootFunction, functions, [](const FunctionPtr& f){});
        }

        // Recursively traverses the Function graph underlying the 'rootFunction' to determine all the leaves (aka inputs) of the graph
        static std::vector<Variable> DetermineInputs(const FunctionPtr& rootFunction, std::unordered_set<FunctionPtr>& visitedFunctions)
        {
            vector<FunctionPtr> functions;
            std::vector<Variable> inputs;
            std::unordered_set<Variable> uniqueInputs;
            Traverse(rootFunction, visitedFunctions, [&inputs, &uniqueInputs](const FunctionPtr& f) { 
                std::vector<Variable> functionInputs = f->Inputs();
                for (auto input : functionInputs)
                {
                    if (!input.IsOutput() && uniqueInputs.find(input) == uniqueInputs.end()) 
                    {
                        inputs.push_back(input);
                        uniqueInputs.insert(input);
                    }
                }
            });

            return inputs;
        }

        // If the network is already created, copy internal state over from the functions in the graph into the underlying network.
        void UpdateInternalNetworkState();

        // Copy state info from source function graph into' this' function graph.
        void CopyState(const CompositeFunction& source);

        template <typename ElementType>
        Microsoft::MSR::CNTK::ComputationNetworkPtr GetComputationNetwork(const DeviceDescriptor& device,
                                                                          const std::unordered_set<Variable>& backpropRoots,
                                                                          const std::unordered_set<Variable>& outputs,
                                                                          bool allocateNetworkMatrices);

        template <typename ElementType>
        static Microsoft::MSR::CNTK::ComputationNodeBasePtr CreateComputationNode(const Variable& variable,
                                                                                  Function* function,
                                                                                  const std::vector<std::shared_ptr<Microsoft::MSR::CNTK::ComputationNode<ElementType>>>& inputNodes,
                                                                                  Microsoft::MSR::CNTK::ComputationNetworkPtr& network,
                                                                                  std::unordered_map<Variable, Microsoft::MSR::CNTK::ComputationNodeBasePtr>& variableToNodeMap);

        template <typename ElementType>
        static Microsoft::MSR::CNTK::ComputationNodeBasePtr GetOutputVariableNode(const Variable& variable,
                                                                                  Microsoft::MSR::CNTK::ComputationNetworkPtr& network,
                                                                                  Microsoft::MSR::CNTK::ComputationNetworkBuilder<ElementType>& builder,
                                                                                  std::unordered_map<Variable, Microsoft::MSR::CNTK::ComputationNodeBasePtr>& variableToNodeMap,
                                                                                  std::unordered_map<Variable, bool>& isVariableRootMap);

        template <typename ElementType>
        static Microsoft::MSR::CNTK::ComputationNodeBasePtr GetNode(const Variable& variable, Microsoft::MSR::CNTK::ComputationNetworkPtr& network,
                                                                    Microsoft::MSR::CNTK::ComputationNetworkBuilder<ElementType>& builder,
                                                                    std::unordered_map<Variable, Microsoft::MSR::CNTK::ComputationNodeBasePtr>& variableToNodeMap,
                                                                    std::unordered_map<Variable, bool>& isVariableRootMap);

        template <typename ElementType>
        static void PopulateComputationNodeValue(const std::pair<Variable, ValuePtr>& variableValue, Microsoft::MSR::CNTK::ComputationNodeBasePtr& computationNode);
        void PopulateNetworkInputs(const std::unordered_map<Variable, ValuePtr>& arguments);

        template <typename ElementType>
        static void PopulateComputationNodeGradient(const std::pair<Variable, ValuePtr>& variableGradient, Microsoft::MSR::CNTK::ComputationNodeBasePtr& computationNode);
        void PopulateNetworkGradients(const std::unordered_map<Variable, ValuePtr>& gradients);

        static void GetNodeOutputOrGradient(Variable var, ValuePtr& varValue, Microsoft::MSR::CNTK::ComputationNodeBasePtr& computationNode, bool getGradient);
        void GetNetworkOutputs(std::unordered_map<Variable, ValuePtr>& outputs);
        void GetNetworkGradients(std::unordered_map<Variable, ValuePtr>& gradients);

        const std::vector<Variable>& GetArgumentDependencies(const Variable& output);

        std::unordered_map<Variable, uint64_t> GetCurrentBackpropRootsTimeStamps() const;

    private:

        // Set of all primitive functions in the graph underlying 'this' Function. Also keeps the primitive Function objects alive 
        // by holding strong references to them
        std::unordered_set<FunctionPtr> m_allPrimitiveFunctions;

        // A map from Variable objects to ComputationNode objects in the ComputationNetwork instance that implements 'this' Composite Function
        std::unordered_map<Variable, Microsoft::MSR::CNTK::ComputationNodeBasePtr> m_variableToNodeMap;

        // A map that tells whether a Variable in the graph underlying 'this' Function is a root of the graph
        std::unordered_map<Variable, bool> m_isVariableRootMap;

        Microsoft::MSR::CNTK::ComputationNetworkPtr m_computationNetwork;

        // The backpropRoots sepecified in the most recent 'Forward' call on 'this' Function.
        // This indicates for which of its roots has 'this' Function retained required intermediate 
        // states from the previos Forward call to be able to backpropagate gradients backwards from in
        // the next 'Backward' call.
        std::unordered_set<Variable> m_currentBackpropRoots;

        // The outputs specified in the most recent 'Forward' call on 'this' Function/
        // This indicates which outputs has the memory sharing structure of the cached
        // computation network object being setup for. Asking for outputs in subsequent
        // 'Forward' calls that do not belong to the current set required redoing the 
        // network memory sharing structure.
        std::unordered_set<Variable> m_currentOutputs;

        std::unordered_map<Variable, std::vector<Variable>> m_perOutputVarArgumentDependencies;

        bool m_networkMatricesAllocated;

        std::unordered_map<Parameter, size_t> m_lastRecordedParameterValueTimeStamps;

        // Version history:
        // 1 -- initial version.
        // 2 -- add support for stateful functions (with corresponding nodes inheriting from RngUser).
        static const size_t s_serializationVersion = 2;
    };

    inline std::vector<CNTK::Axis> DynamicAxesFromInternalDynamicAxisName(const std::wstring& internalDynamicAxisName)
    {
        std::vector<CNTK::Axis> inputVarDynamicAxes;
        if (internalDynamicAxisName.substr(0, CNTK::CompositeFunction::InternalDefaultDynamicAxisName.length()) == CNTK::CompositeFunction::InternalDefaultDynamicAxisName)
            inputVarDynamicAxes = { CNTK::Axis::DefaultDynamicAxis(), CNTK::Axis::DefaultBatchAxis() };
        else if (internalDynamicAxisName.substr(0, CNTK::CompositeFunction::InternalNoSequenceAxisName.length()) == CNTK::CompositeFunction::InternalNoSequenceAxisName)
            inputVarDynamicAxes = { CNTK::Axis::DefaultBatchAxis() };
        else
            inputVarDynamicAxes = { CNTK::Axis(internalDynamicAxisName), CNTK::Axis::DefaultBatchAxis() };

        return inputVarDynamicAxes;
    }

    // Construct the dynamic axis name to be used internally for the CNTK InputNodes
    inline std::wstring InternalDynamicAxisNameFromDynamicAxes(const std::vector<Axis>& dynamicAxes)
    {
        if (dynamicAxes.empty())
            LogicError("Empty dynamic axes set");

        if (dynamicAxes == std::vector<Axis>({ Axis::DefaultBatchAxis() }))
            return CompositeFunction::InternalNoSequenceAxisName;
        else if (dynamicAxes == std::vector<Axis>({ Axis::DefaultDynamicAxis(), Axis::DefaultBatchAxis() }))
            return CompositeFunction::InternalDefaultDynamicAxisName;
        else
            return dynamicAxes[0].Name();
    }
}
