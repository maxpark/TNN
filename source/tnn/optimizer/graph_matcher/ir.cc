// Tencent is pleased to support the open source community by making TNN available.
//
// Copyright (C) 2020 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "tnn/optimizer/graph_matcher/ir.h"

#include <stack>
#include <list>
#include <set>

#include "tnn/core/macro.h"
#include "tnn/core/status.h"
#include "tnn/optimizer/graph_matcher/lexer.h"
#include "tnn/optimizer/graph_matcher/graph_matcher.h"
#include "tnn/optimizer/graph_matcher/logger.h"

namespace TNN_NS {

#define NODE_TEST(expr)                                                                 \
    if (!(expr)) {                                                                      \
        char _ss[2000];                                                                 \
        snprintf(_ss, 2000, "\t\tSanity Check failed on expr "#expr" of node [%s]",     \
                this->name().c_str());                                                  \
        ERROR("%s", _ss);                                                               \
        return Status(TNNERR_COMMON_ERROR, _ss);                                        \
    }


    static auto updateVector = [](std::vector<std::string> &v, const std::string &old_name, const std::string &new_name) {
        for(auto it = v.begin();it!=v.end();it++)  {
            if (*it == old_name) {
                DEBUG("\t\tupdateted origin:%s to:%s", old_name.c_str(), new_name.c_str());
                *it = new_name;
            }
        }
    };

    static auto updateSet= [](std::set<std::string> &v, const std::string &old_name, const std::string &new_name) {
        auto it = v.find(old_name);
        if (it != v.end()) {
            v.erase(old_name);
            v.insert(new_name);
        }
    };

    Edge::Edge(Node * _src, Node * _dst, const std::string &_blob) : src(_src), dst(_dst), tensor_name(_blob) {}

    Node::Node(std::shared_ptr<LayerInfo> &layer_info) {
        info = layer_info;
    }

    Node::Node(const std::string &tensor_name) {
        // create placeholder node 
        info = std::make_shared<LayerInfo>();
        info->type = LAYER_PLACEHOLDER;
        info->name = tensor_name;
        info->outputs = {tensor_name};
    }

    Status Node::addOutputEdge(Edge * e) {
        if (e->src != this) {
            ERRORV("invalid output Edge.", msg);
            return Status(TNNERR_COMMON_ERROR, msg);
        }
        output_edges.push_back(e);
        return TNN_OK;
    }

    Status Node::addInputEdge(Edge * e) {
        if (e->dst != this) {
            ERRORV("invalid input Edge[%s].", msg, e->tensor_name.c_str());
            return Status(TNNERR_COMMON_ERROR, msg);
        }
        input_edges.push_back(e);
        return TNN_OK;
    }

    Status Node::addInput(Edge * e) {
        RETURN_IF_FAIL(addInputEdge(e));
        info->inputs.push_back(e->tensor_name);
        return TNN_OK;
    }

    Status Node::updateInput(const std::string &name, const std::string &new_name, Edge * e) {
        if (std::find(info->inputs.begin(), info->inputs.end(), name) == info->inputs.end()) {
            ERRORV("input tensor[%s] not found in Node[%s]'s inputs.", msg, name.c_str(), info->name.c_str());
            return Status(TNNERR_COMMON_ERROR, msg);
        }
        if (std::find_if(input_edges.begin(), input_edges.end(), [&](Edge * ptr){return ptr->tensor_name == name;}) == input_edges.end()) {
            ERRORV("input edge not found in Node input_edges.", msg);
            return Status(TNNERR_COMMON_ERROR, msg);
        };
        for(auto it = info->inputs.begin(); it != info->inputs.end(); it++) {
            if (*it == name) {
                *it = new_name;
            }
        }
        for(auto it = input_edges.begin(); it != input_edges.end(); it++) {
            if ((*it)->tensor_name == name) {
                *it = e;
            }
        }
        e->tensor_name = new_name;
        return TNN_OK;
    }

    Status Node::sanityCheck() {
        NODE_TEST(info->inputs.size() == input_edges.size());
        for(size_t i=0;i<info->inputs.size();i++) {
            NODE_TEST(info->inputs[i] == input_edges[i]->tensor_name);
            NODE_TEST(input_edges[i]->src != nullptr);
            NODE_TEST(input_edges[i]->dst == this);
        }

        auto validOutput = [&](const std::string &name) -> bool {
            return std::find(info->outputs.begin(), info->outputs.end(), name) != info->outputs.end();
        };

        for(size_t i=0;i<output_edges.size();i++) {
            NODE_TEST(validOutput(output_edges[i]->tensor_name));
            NODE_TEST(output_edges[i]->src == this);
            NODE_TEST(output_edges[i]->dst != nullptr);
        }

        if (info->type == LAYER_PLACEHOLDER) {
            NODE_TEST(info->inputs.size() == 0);
            NODE_TEST(info->outputs.size() == 1);
        }

        return TNN_OK;
    }

    // Status Graph::fromNetStructure(std::vector<std::shared_ptr<LayerInfo> > layers) {
    Status Graph::fromInterpreted(NetStructure * structure, NetResource * resource) {
        if (!structure || ! resource) {
            return Status(TNNERR_PARAM_ERR, "got nullptr from Interpreted tnn model.");
        }
        *this = Graph();
        tnn_structure = structure;
        tnn_resource = resource;
        for (auto &p : tnn_structure->inputs_shape_map) {
            auto t = std::make_shared<Tensor>(p.first);
            t->dims = p.second;
            tensors.push_back(t);
            auto n = getNodeOrCreatePlaceHolder(p.first);
        }

        for (auto &p : tnn_structure->input_data_type_map) {
            auto t = getTensorByName(p.first);
            if (!t) {
                ERRORV("Found unknown blob [%s] in input_data_type_map", msg, p.first.c_str());
                return Status(TNNERR_PARAM_ERR, msg);
            }
            t->data_type = p.second;
            auto n = getNodeOrCreatePlaceHolder(p.first);
        }

        if (tnn_resource) {
            for(auto &p : tnn_resource->constant_map) {
                auto t = std::make_shared<Tensor>(p.first);
                t->dims = p.second->GetBufferDims();
                t->data_type = p.second->GetDataType();
                tensors.push_back(t);
                auto n = getNodeOrCreatePlaceHolder(p.first);
            }
        }

        for (auto layer : tnn_structure->layers) {
            auto node = std::make_shared<Node>(layer);
            nodes.push_back(node);
            for (auto out : layer->outputs) {
                if (tensor_2_node.find(out) != tensor_2_node.end()) {
                    ERRORV("duplicated tensor_name found.", msg);
                    return Status(TNNERR_COMMON_ERROR ,msg);
                }
                tensor_2_node[out] = node;
            }
            for (auto in : layer->inputs) {
                auto n = getNodeByTensorName(in);
                if (!n) {
                    ERRORV("Found unknown blob [%s] at Node [%s]", msg, in.c_str(), node->name().c_str());
                    return Status(TNNERR_PARAM_ERR, msg);
                }
                auto e = std::make_shared<Edge>(n.get(), node.get(), in);
                RETURN_IF_FAIL(n->addOutputEdge(e.get()));
                RETURN_IF_FAIL(node->addInputEdge(e.get()));
                edges.push_back(e);
            }
        }

        RETURN_IF_FAIL(reBuildTensorIndex());

        for (auto name : tnn_structure->outputs) {
            auto n = getNodeByTensorName(name);
            if (!n) {
                ERRORV("Found unknown blob [%s] in netstructure->outputs", msg, name.c_str());
                return Status(TNNERR_PARAM_ERR, msg);
            }
        }
        return TNN_OK;
    }

    Graph::Graph(std::string proto_str) {
        // TODO impl, parse a subgraph from prototext, 
        // Could be used as pattern for GraphRewriter 
    }

    std::shared_ptr<Node> Graph::getNodeOrCreatePlaceHolder(const std::string &tensor_name) {
        if (tensor_2_node.find(tensor_name) != tensor_2_node.end()) {
            return tensor_2_node[tensor_name];
        }
        auto input = std::make_shared<Node>(tensor_name);
        placeholders.push_back(input);
        RAISE_ON_ERROR(buildNodeTensorIndex(input));
        return input;
    }

    std::shared_ptr<Node> Graph::getNodeByTensorName(const std::string &tensor_name) const {
        if (tensor_2_node.find(tensor_name) != tensor_2_node.end()) {
            return tensor_2_node.at(tensor_name);
        }
        return nullptr;
    }
    std::shared_ptr<Tensor> Graph::getTensorByName(const std::string &tensor_name) const {
        if (tensor_map.find(tensor_name) != tensor_map.end()) {
            return tensor_map.at(tensor_name);
        }
        auto it = std::find_if(tensors.begin(), tensors.end(), [&](const std::shared_ptr<Tensor> & t) {
                                    return t->name == tensor_name;
                                });
        if (it != tensors.end()) {
            return *it;
        }
        return nullptr;
    }

    Status Graph::addNode(const std::shared_ptr<Node> &n) {
        RETURN_ON_NEQ(n->sanityCheck(), TNN_OK);
        nodes.push_back(n);
        RETURN_ON_NEQ(buildNodeTensorIndex(n), TNN_OK);
        return TNN_OK;
    }

    Status Graph::createNode(const LayerType &type, const std::vector<std::string> &in_names, 
                            const std::vector<std::string> &out_names) {
        if (out_names.size() == 0) {
            ERRORV("you must specify at least one output.", msg);
            return Status(TNNERR_COMMON_ERROR, msg);
        }

        std::vector<std::shared_ptr<Edge>> new_edges;

        for(auto & in : in_names) {
            auto src = getNodeByTensorName(in);
            if (!src) {
                ERRORV("specified input not found.", msg);
                return Status(TNNERR_COMMON_ERROR, msg);
            }
        }

        for(auto & in : out_names) {
            auto src = getNodeByTensorName(in);
            if (src) {
                ERRORV("specified output alread exists.", msg);
                return Status(TNNERR_COMMON_ERROR, msg);
            }
        }

        // all check passed, now make changes.
        if (type == LAYER_PLACEHOLDER) {
            if (in_names.size() > 0 || out_names.size() != 1) {
                return Status(TNNERR_COMMON_ERROR, "invalid placeholder configuration.");
            }
            auto new_node = getNodeOrCreatePlaceHolder(*out_names.begin());
            if (!new_node) {
                return Status(TNNERR_COMMON_ERROR, "create placeholder failed.");
            }
            return TNN_OK;
        }

        auto new_node = std::make_shared<Node>(out_names[0]);
        new_node->info->type = type;
        new_node->info->outputs = out_names;

        for(auto & in : in_names) {
            auto src = getNodeByTensorName(in);
            auto e = std::make_shared<Edge>(src.get(), new_node.get(), in);
            src->addOutputEdge(e.get());
            new_node->addInput(e.get());
            edges.push_back(e);
        }
        RETURN_IF_FAIL(addNode(new_node));
        return TNN_OK;
    }

    Status Graph::markOutput(const std::string &tensor_name) {
        if (tensor_map.find(tensor_name) == tensor_map.end()) {
            ERRORV("specified tensor [%s] not found.", msg, tensor_name.c_str());
            return Status(TNNERR_COMMON_ERROR, msg);
        }
        marked_outputs.insert(tensor_name);
        return TNN_OK;
    }

    const std::vector<std::weak_ptr<const Node>> Graph::allNodes() const {
        return std::vector<std::weak_ptr<const Node>>(nodes.begin(), nodes.end());
    }

    Status Graph::buildNodeTensorIndex(const std::shared_ptr<Node> n) {
        RETURN_ON_NEQ(n->sanityCheck(), TNN_OK);

        for (auto out : n->info->outputs) {
            if (tensor_2_node.find(out) != tensor_2_node.end()) {
                ERRORV("duplicated tensor_name [%s] found at Node [%s].", msg, out.c_str(), n->name().c_str());
                return Status(TNNERR_COMMON_ERROR, msg);
            }
            tensor_2_node[out] = n;

            if (tensor_map.find(out) != tensor_map.end()) {
                ERRORV("duplicated tensors found.", msg);
                return Status(TNNERR_COMMON_ERROR, msg);
            }
            auto t = getTensorByName(out);
            if (!t) {
                t = std::make_shared<Tensor>(out);
                tensors.push_back(t);
            }
            tensor_map[out] =  t;
        } 
        for (size_t i=0;i<n->input_edges.size();i++) {
            if (tensor_2_node.find(n->info->inputs[i]) == tensor_2_node.end()) {
                ERRORV("input node [%s] not found.", msg, n->info->inputs[i].c_str());
                return Status(TNNERR_COMMON_ERROR, msg);
            }
            if (tensor_map.find(n->info->inputs[i]) == tensor_map.end()) {
                ERRORV("input tensor [%s] not found.", msg, n->info->inputs[i].c_str());
                return Status(TNNERR_COMMON_ERROR, msg);
            }
            tensor_2_edge[n->info->inputs[i]].push_back(n->input_edges[i]);
        }
        return TNN_OK;
    }

    Status Graph::reBuildTensorIndex() {
        tensor_2_node.clear();
        tensor_2_edge.clear();
        tensor_map.clear();

        // skip AnchorGraph 
        if (dynamic_cast<AnchorGraph*>(this) == nullptr) {
            RETURN_IF_FAIL(topologicalSort());
        }

        for(auto &n : placeholders) {
            RETURN_IF_FAIL(buildNodeTensorIndex(n));
        }

        for(auto &n : nodes) {
            RETURN_IF_FAIL(buildNodeTensorIndex(n));
        }
        return sanityCheck();
        // return TNN_OK;
    }
    
    Status Graph::sanityCheck() {
        for(auto &n : placeholders) {
            RETURN_IF_FAIL(n->sanityCheck());
        }

        for(auto &n : nodes) {
            RETURN_IF_FAIL(n->sanityCheck());

            size_t total = 0;
            for(auto &name : n->info->outputs) {
                if (tensor_2_edge.count(name) > 0) {
                    total += tensor_2_edge.at(name).size();
                }
            }
            if (n->output_edges.size() != total) {
                ERRORV("number of output edges [%lu] not match tensor_usage:[%lu] for node[%s].", \
                        msg, n->output_edges.size(), total, n->name().c_str());
                return Status(TNNERR_COMMON_ERROR, msg);
            }
        }
        return TNN_OK;
    }

    std::vector<const Tensor*> Graph::getTensorsByNames(const std::vector<std::string> &names) const  {
        std::vector<const Tensor*> res;
        for(auto &name : names) {
            auto tensor = getTensorByName(name);
            if (!tensor) {
                ERRORV("got unkonwn tensor [%s].", msg, name.c_str());
                throw std::runtime_error(msg);
            }
            res.push_back(tensor.get());
        }
        return res;
    }

    std::vector<const Tensor*> Graph::outputs() const {
        std::set<std::string> names = marked_outputs;

        for(auto &n : nodes) {
            RAISE_ON_ERROR(n->sanityCheck());
            if (n->output_edges.size() == 0) {
                for(auto &name : n->info->outputs)
                    names.insert(name);
            }
        }
        return getTensorsByNames(std::vector<std::string>(names.begin(), names.end()));
    }

    std::vector<const Tensor*> Graph::inputs() const {
        std::set<std::string> names;

        for(auto &n : placeholders) {
            RAISE_ON_ERROR(n->sanityCheck());
            if (n->output_edges.size() > 0) {
                names.insert(n->info->outputs[0]);
            }
        }
        return getTensorsByNames(std::vector<std::string>(names.begin(), names.end()));
    }

    Status Graph::rewrite(std::shared_ptr<Graph> &pattern, graph_generator generator) {
        try { 
            RAISE_ON_ERROR(sanityCheck());

            std::vector<std::shared_ptr<AnchorGraph>> matches;
            match(shared_from_this(), pattern, matches);

            auto groups = clustering(matches);

            INFO("matched subgraphs:%lu groups:%lu", matches.size(), groups.size());

            for(auto &group : groups) {
                if (group.size() != 1) {
                    // currently, only restricted mode is supported. 
                    // which means the pattern should not has multiple isomorphic matches.
                    WARN("a group of %lu matched SubGraphs are ignored because those subgraphs share some common nodes.", group.size());
                    continue;
                }
                auto &origin_graph = group[0];
                // previus changes may cause tensor_name changes, so rebuild index here.
                origin_graph->formalize(this);

                auto heir_graph = generator(origin_graph);
                if (heir_graph) {

                    if (heir_graph->inputs().size() != origin_graph->inputs().size()) {
                        WARN("Warning: Skiped one replacement. heir_graph and origin graph num inputs not match, %lu != %lu", 
                                    heir_graph->inputs().size(),  origin_graph->inputs().size());
                        continue;
                    }
                    if (heir_graph->outputs().size() != origin_graph->outputs().size()) {
                        WARN("Warning: Skiped one replacement. heir_graph and origin graph num outputs not match, %lu != %lu", 
                                    heir_graph->outputs().size(),  origin_graph->outputs().size());
                        continue;
                    }
                    if (heir_graph->reBuildTensorIndex() != TNN_OK) {
                        WARN("Warning: the generated graph is not valid. skip now");
                        continue;
                    }
                    // TODO topological sort the inputs and outputs of the two graphs.
                    heir_graph->embed(shared_from_this(), origin_graph, std::string("_rewrited_") + std::to_string(rewrite_count++) + std::string("_"));
                    INFO("replaced an AnchorGraph with HeirGraph");
                } else {
                    WARN("Pattern not replaced.");
                }
            }

        } catch (const std::exception& error) {
            ERROR("%s", error.what());
            return Status(TNNERR_COMMON_ERROR, error.what());
        } catch (const std::string & e) {
            ERROR("%s", e.c_str());
            return Status(TNNERR_COMMON_ERROR, e.c_str());
        } catch (...) {
            std::exception_ptr eptr = std::current_exception();
            ERRORV("Rewriter got unknow error.", msg);
            return Status(TNNERR_COMMON_ERROR, msg);
        }

        return TNN_OK;
    }

    void Graph::dump(std::ostream &os) const {
        // line 1 header line: 1 num_blobs 1 magic_number
        // !!!assume each node has only one output here.
        os << "\"1 " << tensor_2_node.size() << " 1 4206624772 ,\"\n";
        // line 2 inputs: ':'.join(name rank dims dtype)
        auto input_tensors = inputs();
        auto it = input_tensors.begin();
        os << "\"" << (*it)->name << " 0 0 ";
        for(it++;it!=input_tensors.end();it++) {
            os << ": " << (*it)->name << " 0 0 ";
        }
        os << " ,\"\n\"";
        // line 3 blobs
        for(auto &n : placeholders) {
            for (auto &blob : n->info->outputs) 
                os << blob << " ";
        }
        for(auto &n : nodes) {
            for (auto &blob : n->info->outputs) 
                os << blob << " ";
        }
        os << " ,\"\n\"";
        // line 4 outptus
        for(auto &v : outputs()) {
            os << v->name << " ";
        }
        os << " ,\"\n\"";
        // line 5 num layers
        os << nodes.size() << " ,\"\n";
        // layers
        for(auto &n : nodes) {
            os << "\"" << layerTypeName(n->info->type) << " " << n->name();
            os << " " << n->info->inputs.size() << " " << n->info->outputs.size() << " ";
            for(auto &in : n->info->inputs) { os << in << " "; }
            for(auto &out : n->info->outputs) { os << out << " "; }
            os << " ,\"\n";
        }

    }

    Status Graph::topologicalSort() {
        std::set<std::string> known_names;
        std::list<std::shared_ptr<Node>> pool;
        std::vector<std::shared_ptr<Node>> sorted;
        sorted.reserve(nodes.size());

        for(auto &n : placeholders) {
            for(auto &name : n->info->outputs) 
                known_names.insert(name);
        }

        auto all_names_known = [&](const std::vector<std::string> &names) -> bool {
            return std::find_if(names.begin(), names.end(), 
                                [&](const std::string &name){ return known_names.count(name) == 0; }) == names.end();
        };

        auto testAndPush = [&](const std::shared_ptr<Node> &n) -> bool {
            if (all_names_known(n->info->inputs)) {
                for(auto &name : n->info->outputs) {
                    known_names.insert(name);
                }
                sorted.push_back(n);
                return true;
            }
            return false;
        };

        for(auto &n : nodes) {
            if (!testAndPush(n)) {
                pool.push_back(n);
            }
        }

        size_t last_size = 0;
        while( pool.size() > 0 && last_size != pool.size() ) {
            last_size = pool.size();
            pool.erase(std::remove_if(pool.begin(), pool.end(), [&](const std::shared_ptr<Node> &n) {
                                        return testAndPush(n);
                                    }), pool.end());
        }

        if (pool.size() != 0) {
            return Status(TNNERR_COMMON_ERROR, "Got invalid graph, eg. cycled graph.");
        }

        nodes = sorted;

        return TNN_OK;
    }


    Status Graph::renameTensor(const std::string &old_name, const std::string &new_name) {
        for(auto &n : placeholders) {
            updateVector(n->info->inputs, old_name, new_name);
            updateVector(n->info->outputs, old_name, new_name);
        }

        for(auto &n : nodes) {
            updateVector(n->info->inputs, old_name, new_name);
            updateVector(n->info->outputs, old_name, new_name);
        }

        for(auto &e : edges) {
            if (e->tensor_name == old_name) {
                e->tensor_name = new_name;
            }
        }

        for(auto &t : tensors) {
            if (t->name == old_name) {
                t->name = new_name;
            }
        }

        updateSet(marked_outputs, old_name, new_name);
        if (tnn_structure) {
            updateSet(tnn_structure->blobs, old_name, new_name);
            updateSet(tnn_structure->outputs, old_name, new_name);
        }

        return reBuildTensorIndex();;
    }

    void Graph::embed(std::shared_ptr<Graph> g, const std::shared_ptr<AnchorGraph> anchor, std::string name_prefix) {
        // 1. check every edge has a replacement tensor
        // 2. remove all inEges.
        // 3. connect new_inEdges to g
        // 3. replace all outEdges->src to new_graph.
        // 5. remove unused Nodes

        std::set<std::string> tensor_names;
        for(auto & p : tensor_map) tensor_names.insert(p.first);
        for(auto &name : tensor_names) renameTensor(name, name_prefix + name);

        std::map<std::string, std::string> in_mapping;
        for(size_t i=0;i<anchor->inputs().size();i++) {
            in_mapping[anchor->inputs()[i]->name] = inputs()[i]->name;
            in_mapping[inputs()[i]->name] =anchor->inputs()[i]->name;
        }

        std::map<std::string, std::string> out_mapping;
        for(size_t i=0;i<anchor->outputs().size();i++) {
            out_mapping[anchor->outputs()[i]->name] = outputs()[i]->name;
        }

        // check first
        for(auto & e : anchor->inEdges()) {
            if (in_mapping.count(e->tensor_name) == 0) {
                ERRORV("Input edge [%s] of the subgraph not found replacement.", msg, e->tensor_name.c_str());
                throw std::runtime_error(msg);
            }
            auto src_node = g->getNodeByTensorName(e->tensor_name);
            if (!src_node) {
                ERRORV("src node of inEdge [%s] not found.", msg, e->tensor_name.c_str());
                throw std::runtime_error(msg);
            }
        }

        for(auto & e : anchor->outEdges()) {
            if (out_mapping.find(e->tensor_name) == out_mapping.end()) {
                ERRORV("Output edge of the subgraph not found replacement.", msg);
                throw std::runtime_error(msg);
            }
            auto new_node = getNodeByTensorName(out_mapping.at(e->tensor_name));
            if (!new_node) {
                ERRORV("New node of output edge not found.", msg);
                throw std::runtime_error(msg);
            }
        }


        // the inEdges and outEdge is calculated according to the edges. 
        // so make a copy here before changing the graph.
        auto in_edges = anchor->inEdges();
        auto out_edges = anchor->outEdges();

        // number of input_edegs of new_graph might less than that of the old graph.

        for(auto & e : in_edges) {
            e->dst->input_edges.erase(std::remove_if(e->dst->input_edges.begin(), e->dst->input_edges.end(), [&](Edge * cur){
                                            return cur->src == e->src;
                                        }), e->dst->input_edges.end());
            e->src->output_edges.erase(std::remove_if(e->src->output_edges.begin(), e->src->output_edges.end(), [&](Edge * cur){
                                            return cur->dst == e->dst;
                                        }), e->src->output_edges.end());
            g->edges.erase(std::remove_if(g->edges.begin(), g->edges.end(), [&](const std::shared_ptr<Edge> &edge) {
                                return edge.get() == e;
                            }), g->edges.end());
        }

        for(auto & n: nodes) {
            for(auto &e: n->input_edges) {
                if (in_mapping.count(e->tensor_name) > 0) {
                    DEBUG("Updating input from %s -> %s for Node[%s]", e->tensor_name.c_str(), in_mapping[e->tensor_name].c_str(), n->name().c_str());
                    auto src_node = g->getNodeByTensorName(in_mapping[e->tensor_name]);
                    e->src = src_node.get();

                    RAISE_ON_ERROR(n->updateInput(e->tensor_name, in_mapping.at(e->tensor_name), e));
                    RAISE_ON_ERROR(src_node->addOutputEdge(e));
                }
            }
        }

        for(auto & e : out_edges) {
            Node * old_node = e->src;
            Node * new_node = getNodeByTensorName(out_mapping[e->tensor_name]).get();
            old_node->output_edges.erase(std::remove_if(old_node->output_edges.begin(), old_node->output_edges.end(), [&](Edge * cur){
                                            return cur->dst == e->dst;
                                        }), old_node->output_edges.end());
            auto old_name = e->tensor_name;
            auto new_name = out_mapping[e->tensor_name];
            e->src = new_node;
            DEBUG("Updating input from %s -> %s for Node[%s]", old_name.c_str(), new_name.c_str(), e->dst->name().c_str());
            RAISE_ON_ERROR(e->dst->updateInput(old_name, new_name, e));
            RAISE_ON_ERROR(new_node->addOutputEdge(e));
        }

        // Update marked_outptus, net_structure->blobs, net_structure->outputs
        for(auto &p : out_mapping) {
            updateSet(marked_outputs, p.first, p.second);
            if (g->tnn_structure) {
                updateSet(g->tnn_structure->blobs, p.first, p.second);
                updateSet(g->tnn_structure->blobs, p.first, p.second);
            }
        }

        // remove the deleted nodes, tensors, net_structure->blobs
        for(auto it = g->nodes.begin(); it!= g->nodes.end(); ) {
            if (std::find(anchor->nodes.begin(), anchor->nodes.end(), *it) != anchor->nodes.end()) {
                for(auto &name : (*it)->info->outputs) {
                    g->tensors.erase(std::remove_if(g->tensors.begin(), g->tensors.end(), [&](const std::shared_ptr<Tensor> &t){
                        return t->name == name;
                    }), g->tensors.end());
                    if (g->tnn_structure) g->tnn_structure->blobs.erase(name);
                }
                it = g->nodes.erase(it);
            } else {
                ++it;
            }
        }

        g->nodes.insert(g->nodes.end(), nodes.begin(), nodes.end());
        g->edges.insert(g->edges.end(), edges.begin(), edges.end());

        RAISE_ON_ERROR(g->reBuildTensorIndex());

        if (g->tnn_structure) {
            // add new blobs to tnn_structure
            for (auto &n : nodes) {
                for(auto &name : n->info->outputs) {
                    g->tnn_structure->blobs.insert(name);
                }
            }
            
            // update net_structure
            std::vector<std::shared_ptr<LayerInfo>> new_layers;
            for(auto &n: g->nodes) {
                new_layers.push_back(n->info);
            }
            g->tnn_structure->layers = new_layers;
        }

    }
}
