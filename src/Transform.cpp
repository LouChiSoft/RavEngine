#include "Transform.hpp"
#include "mathtypes.hpp"
#include <glm/gtc/type_ptr.hpp>
#include "Common3D.hpp"

using namespace std;
using namespace glm;
using namespace RavEngine;

Transform& Transform::AddChild(ComponentHandle<Transform> child)
{
    auto cptr = child.get();
	auto worldPos = cptr->GetWorldPosition();
	auto worldRot = cptr->GetWorldRotation();
	
	cptr->parent = ComponentHandle<Transform>(GetOwner());
	children.insert(child);
	child->matrix = GetWorldMatrix();
	
    cptr->SetWorldPosition(worldPos);
    cptr->SetWorldRotation(worldRot);
    return *this;
}

Transform& Transform::RemoveChild(ComponentHandle<Transform> child)
{
    auto cptr = child.get();
	auto worldPos = cptr->GetWorldPosition();
	auto worldRot = cptr->GetWorldRotation();
	cptr->parent.reset();
	children.erase(child);
	child->matrix = matrix4(1);
	cptr->SetWorldPosition(worldPos);
	cptr->SetWorldRotation(worldRot);
    return *this;
}

void RavEngine::Transform::UpdateChildren()
{
	auto update = [](Transform* transform, auto&& updatefn) -> void {
		transform->MarkAsDirty();
		auto newParentMatrix = transform->GetWorldMatrix();
		for (auto& child : transform->children) {
			auto component = child.get();
			component->matrix = newParentMatrix;

			updatefn(component,updatefn);
		}
	};
	update(this, update);	
}
