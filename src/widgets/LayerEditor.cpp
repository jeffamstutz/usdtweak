#include <iostream>
#include <iomanip> // we are doomed
#include <sstream>
#include <array>
#include <cctype>
#include "Gui.h"
#include <pxr/usd/usd/schemaRegistry.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/layerUtils.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/sdf/variantSpec.h>
#include <pxr/usd/sdf/variantSetSpec.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/camera.h>
#include "Editor.h"
#include "Commands.h"
#include "ModalDialogs.h"
#include "FileBrowser.h"
#include "LayerEditor.h"
#include "PrimSpecEditor.h" // should really be PrimSpecWidgets.h
#include "Constants.h"

// TODO:
//  [*] select and remove a sublayer
//  [.] find all prim spec types
//  [*] prim tree extend size to the bottom of the window
//  [*] set scene orientation
//  [ ] add metadata comment
//  [*] set prim is hidden
//  [*] set prim is active
//  [ ] manage variant sets
//  [ ] manage references
//  [ ] create all commands
//  [*] use a menu for references and variants instead of a modal



struct AddSublayer : public ModalDialog {

    AddSublayer(SdfLayerRefPtr &layer) : layer(layer){};

    void Draw() override {
        DrawFileBrowser();
        auto filePath = GetFileBrowserFilePath();

        if (FilePathExists()) {
            ImGui::Text("Import layer: ");
        } else {
            ImGui::Text("Not found: ");
        } // ... other messages like permission denied, or incorrect extension
        ImGui::Text("%s", filePath.c_str());

        if (ImGui::Button("Cancel")) {
            CloseModal();
        }
        ImGui::SameLine();
        if (ImGui::Button("Ok")) {
            if (!filePath.empty()) {
                DispatchCommand<LayerInsertSubLayer>(layer, filePath);
            }
            CloseModal();
        }
    }

    const char *DialogId() const override { return "Import sublayer"; }
    SdfLayerRefPtr layer;
};

// Look for a new name. If prefix ends with a number, it will increase its value until
// a valid name/token is found
static std::string FindNextAvailablePrimName(std::string prefix) {
    // Find number in the prefix
    size_t end = prefix.size() - 1;
    while (end>0 && std::isdigit(prefix[end])) {
        end--;
    }
    size_t padding = prefix.size() - 1 - end;
    const std::string number = prefix.substr(end+1, padding);
    auto value = number.size() ? std::stoi(number) : 0;
    std::ostringstream newName;
    padding = padding == 0 ? 4 : padding; // 4: default padding
    do {
        value += 1;
        newName.seekp(0, std::ios_base::beg); // rewind
        newName << prefix.substr(0, end+1) << std::setfill('0') << std::setw(padding) << value;
        // Looking for existing token with the same name.
        // There might be a better solution here
    } while (TfToken::Find(newName.str())!=TfToken());
    return newName.str();
}

/// Draw a popup to quickly edit a prim
void DrawPrimQuickEdit(SdfPrimSpecHandle &primSpec) {
    if (!primSpec) return;
    if (ImGui::MenuItem("Add child prim")) {
        DispatchCommand<PrimNew>(primSpec, FindNextAvailablePrimName(DefaultPrimSpecName));
    }
    auto parent = primSpec->GetNameParent();
    if (parent) {
        if (ImGui::MenuItem("Add sibling prim")) {
            DispatchCommand<PrimNew>(parent, FindNextAvailablePrimName(primSpec->GetName()));
        }
    }

    if (ImGui::BeginMenu("Add composition arc")) {
        DrawPrimCompositionPopupMenu(primSpec);
        ImGui::EndMenu();
    }

    if (ImGui::MenuItem("Remove")) {
        // TODO: simplify
        DispatchCommand<UsdApiFunction>(primSpec->GetLayer(), std::function<void()> {
            [=]() {
                if (primSpec->GetNameParent()) {
                    primSpec->GetNameParent()->RemoveNameChild(primSpec);
                    return true;
                }
                else {
                    primSpec->GetLayer()->RemoveRootPrim(primSpec);
                    return true;
                    }
                }
        });
    }


    // TODO a function DrawVariantsPopupMenu() instead of the following code ?
    auto variantSetNames = primSpec->GetVariantSets();
    if (!variantSetNames.empty()) {
        ImGui::Separator();
        for (const auto &variantSet : variantSetNames) {
            if (ImGui::BeginMenu(variantSet.first.c_str())) {
                SdfVariantSetSpecHandle variantSetHandle = variantSet.second;
                if (variantSetHandle) {
                    for (const auto &variant : variantSetHandle->GetVariants()) {
                        if (variant && ImGui::MenuItem(variant->GetName().c_str())) {
                            // TODO: as a command
                            ExecuteAfterDraw(&SdfPrimSpec::SetVariantSelection, primSpec, variantSet.first, variant->GetName());
                            //primSpec->SetVariantSelection(variantSet.first, variant->GetName());
                        }
                    }
                    // TODO: highlight the one currently used
                }
                ImGui::EndMenu();
            }
        }
    }

    ImGui::Separator();
    DrawPrimSpecifierCombo(primSpec);
    DrawPrimName(primSpec);
    // Kind: component/assembly, etc add a combo
    // DrawPrimKind(primSpec);
    // ImGui::Text("%s", primSpec->GetKind().GetString().c_str());
    DrawPrimType(primSpec);

    DrawPrimInstanceable(primSpec);
    DrawPrimHidden(primSpec);
    DrawPrimActive(primSpec);
}

/// Draw a node in the primspec tree
static void DrawPrimSpecTreeNode(SdfPrimSpecHandle primSpec, SdfPrimSpecHandle &selectedPrim, int &nodeId) {
    if (!primSpec)
        return;

    ImGui::TableNextRow();

    // Makes the row selectable
    std::string label = "##DPSTN_" + std::to_string(nodeId);
    nodeId++;
    bool selected = primSpec == selectedPrim;
    if (ImGui::Selectable(label.c_str(), selected,
        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap)) {
        selectedPrim = primSpec;
    }
    ImGui::SameLine();

    auto childrenNames = primSpec->GetNameChildren();
    ImGuiTreeNodeFlags nodeFlags =
        childrenNames.empty() && ! primSpec->HasVariantSetNames() ? ImGuiTreeNodeFlags_Leaf : ImGuiTreeNodeFlags_DefaultOpen;
    nodeFlags |= ImGuiTreeNodeFlags_OpenOnArrow;

    // Draw the hierarchy
    // Share the selection state by using the same label


    std::string primSpecName;
    if (primSpec->GetPath().IsPrimVariantSelectionPath()) {
        auto variantSelection = primSpec->GetPath().GetVariantSelection();
        primSpecName = std::string("{") + variantSelection.first.c_str() + ":" + variantSelection.second.c_str() + "}";
        ImGui::PushStyleColor(ImGuiCol_Text, (ImVec4)ImColor::HSV(0.2/7.0f, 0.5f, 0.5f));
    }
    else {
        primSpecName = primSpec->GetPath().GetName();
    }


    auto unfolded = ImGui::TreeNodeEx((const void *)nodeId, nodeFlags, "%s", primSpecName.c_str() );
    if (ImGui::IsItemClicked()) {
        selectedPrim = primSpec;
    }

    // Right click will open the quick edit popup menu
    if (ImGui::BeginPopupContextItem()) {
        DrawPrimQuickEdit(primSpec);
        ImGui::EndPopup();
    }

    ImGui::TableNextCell();

    std::string description = primSpec->GetPath().IsPrimVariantSelectionPath() ? ""
        : TfEnum::GetDisplayName(primSpec->GetSpecifier()) + " " +
        primSpec->GetTypeName().GetString();

    ImGui::Text("%s", description.c_str());

    if (primSpec->GetPath().IsPrimVariantSelectionPath()) {
        ImGui::PopStyleColor();
    }

    // Draw composition summary
    ImGui::TableNextCell();
    DrawPrimCompositionSummary(primSpec);

    // TODO: draw variant details or hidden or active

    if (unfolded) {
        SdfVariantSetsProxy variantSetMap = primSpec->GetVariantSets();
        TF_FOR_ALL(varSetIt, variantSetMap) {
            const SdfVariantSetSpecHandle &varSetSpec = varSetIt->second;
            const SdfVariantSpecHandleVector &variants = varSetSpec->GetVariantList();
            TF_FOR_ALL(varIt, variants) {
                const SdfPrimSpecHandle &variantSpec = (*varIt)->GetPrimSpec();
                DrawPrimSpecTreeNode(variantSpec, selectedPrim, nodeId);
            }
        }

        for (int i = 0; i < childrenNames.size(); ++i) {
            DrawPrimSpecTreeNode(childrenNames[i], selectedPrim, nodeId);
        }
        ImGui::TreePop();
    }
}

void DrawPrimSpecTree(SdfLayerRefPtr layer, SdfPrimSpecHandle &selectedPrim) {
    int nodeId = 0;
    static ImGuiTableFlags tableFlags = ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##DrawPrimSpecTree", 3, tableFlags)) {
        ImGui::TableSetupColumn("Hierarchy");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Arcs");
        for (const auto &child : layer->GetRootPrims()) {
            DrawPrimSpecTreeNode(child, selectedPrim, nodeId);
        }
        ImGui::EndTable();
    }
}

void DrawLayerPrimTree(SdfLayerRefPtr layer, SdfPrimSpecHandle &selectedPrim) {

    if (ImGui::Button("Add root prim")) {
        DispatchCommand<PrimNew>(layer, FindNextAvailablePrimName(DefaultPrimSpecName));
    }

    ImGui::SameLine();

    if (ImGui::Button("Remove selected") && selectedPrim) {
        // TODO: simplify
        DispatchCommand<UsdApiFunction>(layer, std::function<void()> {
            [=]() {
                if (selectedPrim->GetNameParent()) {
                    selectedPrim->GetNameParent()->RemoveNameChild(selectedPrim);
                    return true;
                }
                else {
                    layer->RemoveRootPrim(selectedPrim);
                    return true;
                    }
                }
        });

        selectedPrim = SdfPrimSpecHandle(); // resets selection
    }
    ImGui::PushItemWidth(-1);
    ImGuiWindow *currentWindow = ImGui::GetCurrentWindow();
    ImVec2 sizeArg(0, currentWindow->Size[1] - 100); // TODO: size of the window
    if (ImGui::ListBoxHeader("##empty", sizeArg)) {
        DrawPrimSpecTree(layer, selectedPrim);
        ImGui::ListBoxFooter();
    }
    ImGui::PushItemWidth(0);
}

void DrawDefaultPrim(SdfLayerRefPtr layer) {
    auto rootPrims = layer->GetRootPrims();
    auto defautPrim = layer->GetDefaultPrim();

    if (ImGui::BeginCombo("Defaut Prim", defautPrim.GetString().c_str())) {
        bool isSelected = defautPrim == "";
        if (ImGui::Selectable("", isSelected)) {
            defautPrim = TfToken("");
            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }
        for (const auto &prim : rootPrims) {
            isSelected = (defautPrim == prim->GetNameToken());
            if (ImGui::Selectable(prim->GetName().c_str(), isSelected)) {
                defautPrim = prim->GetNameToken();
            }
            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }

        if (layer->GetDefaultPrim() != defautPrim) {
            if (defautPrim != "")
            {
                ExecuteAfterDraw(&SdfLayer::SetDefaultPrim, layer, defautPrim);

            }
            else {
                ExecuteAfterDraw(&SdfLayer::ClearDefaultPrim, layer);
            }

        }

        ImGui::EndCombo();
    }
}

/// Draw the up axis orientation.
/// It should normally be set by the stage, not the layer, so the code bellow follows what the api is doing
/// inside
void DrawUpAxis(SdfLayerRefPtr layer) {

    VtValue upAxis = layer->GetField(SdfPath::AbsoluteRootPath(), UsdGeomTokens->upAxis);

    std::string upAxisStr("Default");
    if (!upAxis.IsEmpty()) {
        upAxisStr = upAxis.Get<TfToken>().GetString();
    }

    if (ImGui::BeginCombo("Up Axis", upAxisStr.c_str())) {
        bool selected = !upAxis.IsEmpty() && upAxis.Get<TfToken>() == UsdGeomTokens->z;
        if (ImGui::Selectable("Z", selected)) {
            ExecuteAfterDraw(&SdfLayer::SetField<TfToken>, layer, SdfPath::AbsoluteRootPath(), UsdGeomTokens->upAxis, UsdGeomTokens->z);
        }
        selected = !upAxis.IsEmpty() && upAxis.Get<TfToken>() == UsdGeomTokens->y;
        if (ImGui::Selectable("Y", selected)) {
            ExecuteAfterDraw(&SdfLayer::SetField<TfToken>, layer, SdfPath::AbsoluteRootPath(), UsdGeomTokens->upAxis, UsdGeomTokens->y);
        }
        ImGui::EndCombo();
    }
}


void DrawLayerMetadata(SdfLayerRefPtr layer) {

    if (!layer)
        return;

    DrawDefaultPrim(layer);

    // DrawComments();

    DrawUpAxis(layer);

    // Time
    auto startTimeCode = layer->GetStartTimeCode();
    if (ImGui::InputDouble("Start Time Code", &startTimeCode)) {
        ExecuteAfterDraw(&SdfLayer::SetStartTimeCode, layer, startTimeCode);
    }
    auto endTimeCode = layer->GetEndTimeCode();
    if (ImGui::InputDouble("End Time Code", &endTimeCode)) {
        ExecuteAfterDraw(&SdfLayer::SetEndTimeCode, layer, endTimeCode);
    }

    auto framesPerSecond = layer->GetFramesPerSecond();
    ImGui::InputDouble("Frame per second", &framesPerSecond);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        ExecuteAfterDraw(&SdfLayer::SetFramesPerSecond, layer, framesPerSecond);
    }
}

void DrawLayerPrimEdit(SdfLayerRefPtr layer, SdfPrimSpecHandle &primSpec) {
    if (!layer || !primSpec)
        return;
    ImGui::Text("%s", primSpec->GetPath().GetString().c_str());
    DrawPrimSpecifierCombo(primSpec);
    DrawPrimName(primSpec);
    // Kind: component/assembly, etc add a combo
    // ImGui::Text("%s", primSpec->GetKind().GetString().c_str());
    DrawPrimType(primSpec);

    DrawPrimInstanceable(primSpec);
    DrawPrimHidden(primSpec);
    DrawPrimActive(primSpec);
    // DrawPrimComposition(primSpec);
}

void DrawLayerSublayerTree(SdfLayerRefPtr layer, int depth=0) {
    // Tree node doc:
    // https://github.com/ocornut/imgui/issues/581

    // TODO: should we use a usd layer tree structure ?
    if (!layer)
        return;

    ImGuiTreeNodeFlags nodeflags = ImGuiTreeNodeFlags_OpenOnArrow; // TODO selection
    std::vector<std::string> subLayers = layer->GetSubLayerPaths();

    for (auto sublayerpath : subLayers) {
        auto sublayer = SdfFindOrOpenRelativeToLayer(layer, &sublayerpath);
        if (sublayer && sublayer->GetSubLayerPaths().empty()) {
            nodeflags |= ImGuiTreeNodeFlags_Leaf;
        }
        if (sublayer && ImGui::TreeNodeEx(sublayer->GetDisplayName().c_str(), nodeflags)) {
            DrawLayerSublayerTree(sublayer, depth+1);
            ImGui::TreePop();
        }
        // Right click will open the quick edit popup menu only for sublayers of the current layer
        // TODO: having a selection and buttons would be better for user experience
        if (depth == 0 && ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Remove selected sublayer")) {
                DispatchCommand<LayerRemoveSubLayer>(layer, sublayerpath);
            }
            if (ImGui::MenuItem("Move up")) {
                DispatchCommand<LayerMoveSubLayer>(layer, sublayerpath, true);
            }
            if (ImGui::MenuItem("Move down")) {
                DispatchCommand<LayerMoveSubLayer>(layer, sublayerpath, false);
            }
            ImGui::EndPopup();
        }
    }
}

void DrawLayerSublayersPage(SdfLayerRefPtr layer) {
    if (!layer)
        return;
    if (ImGui::Button("Add sublayer")) {
        TriggerOpenModal<AddSublayer>(layer);
    }
    ImGui::PushItemWidth(-1);
    // TODO: Mute and Unmute layers
    if (ImGui::ListBoxHeader("##empty", 10, 10)) {
        DrawLayerSublayerTree(layer);
        ImGui::ListBoxFooter();
    }
    ImGui::PushItemWidth(0);
}

/// Draw a SdfLayer in place editor
void DrawLayerEditor(SdfLayerRefPtr layer, SdfPrimSpecHandle &selectedPrim) {

    if (!layer)
        return;
    if (ImGui::CollapsingHeader("Metadata")) {
         DrawLayerMetadata(layer);
    }
    if (ImGui::CollapsingHeader("Sublayers")) {
         DrawLayerSublayersPage(layer);
    }
    if (ImGui::CollapsingHeader("Prims tree")) {
         DrawLayerPrimTree(layer, selectedPrim);
    }
}
