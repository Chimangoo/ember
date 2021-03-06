//
// C++ Implementation: EntityEditor
//
// Description: 
//
//
// Author: Erik Hjortsberg <erik.hjortsberg@gmail.com>, (C) 2007
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.//
//
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "EntityEditor.h"

#include "components/ogre/MousePicker.h"
#include "components/ogre/Convert.h"
#include "components/ogre/World.h"
#include "domain/EmberEntity.h"
#include "components/ogre/ShapeVisual.h"
#include "components/ogre/Avatar.h"
#include "components/ogre/authoring/Polygon.h"
#include "components/ogre/authoring/PolygonPoint.h"
#include "components/ogre/authoring/IPolygonPositionProvider.h"
#include "components/ogre/terrain/TerrainManager.h"

#include "adapters/atlas/AdapterBase.h"
#include "adapters/atlas/MapAdapter.h"

#include "services/EmberServices.h"
#include "services/server/ServerService.h"
#include "framework/LoggingInstance.h"

#include <Atlas/PresentationBridge.h>
#include <Atlas/Message/Element.h>
#include <Atlas/Message/QueuedDecoder.h>
#include <Atlas/Codecs/XML.h>
#include <Atlas/Codecs/Bach.h>
#include <Atlas/Formatter.h>

#include <wfmath/segment.h>
#include <wfmath/atlasconv.h>

#include <Eris/Entity.h>
#include <Eris/Account.h>
#include <Eris/Connection.h>
#include <Eris/Response.h>
#include <Eris/View.h>

#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreEntity.h>

#include <sigc++/trackable.h>

using namespace Atlas::Message;

namespace Ember
{
namespace OgreView
{

namespace Gui
{

/**
 * @author Erik Ogenvik
 *
 * @brief Provides a marker and a line from an entity to a point.
 *
 * This is useful for visualizing locations known by entities.
 *
 */
class EntityEditor::EntityPointMarker: public virtual sigc::trackable
{
protected:

	/**
	 * @brief The entity to which the marker is connected.
	 */
	Eris::Entity& mEntity;

	/**
	 * @brief The entity which represents the entity.
	 *
	 * This is a large blue ball.
	 */
	Ogre::Entity* mMarkerEntity;

	/**
	 * @brief The node to which the marker entity is attached.
	 */
	Ogre::SceneNode* mMarkerNode;

	/**
	 * @brief A line drawn from the entity to the marker.
	 */
	ShapeVisual* mMarkerDirectionIndicator;

	/**
	 * @brief Provides height data.
	 */
	const IHeightProvider& mHeightProvider;

	/**
	 * @brief The location we want marked.
	 */
	const WFMath::Point<3> mPoint;

public:

	/**
	 * @brief Updates the marker, and any line drawn to it.
	 */
	void updateMarker()
	{
		//Check if we should adjust to the height of the world
		WFMath::Point<3> adjustedPoint(mPoint);

		float height = adjustedPoint.z();
		if (mHeightProvider.getHeight(TerrainPosition(mPoint.x(), mPoint.y()), height)) {
			adjustedPoint.z() = height;
		}

		mMarkerNode->setPosition(Convert::toOgre(adjustedPoint));

		WFMath::Segment<3> shape(adjustedPoint, mEntity.getViewPosition() + WFMath::Vector<3>(mEntity.getBBox().getCenter()));
		mMarkerDirectionIndicator->update(shape);

	}

	/**
	 * @brief Called when the entity moves.
	 */
	void entityMoved()
	{
		updateMarker();
	}

	/**
	 * @brief Ctor.
	 * @param entity The entity which the marker is attached to.
	 * @param sceneManager A scene manager used to create nodes and entities.
	 * @param terrainManager Provides height data.
	 * @param point The location which will be marked.
	 */
	EntityPointMarker(Eris::Entity& entity, Ogre::SceneManager& sceneManager, const IHeightProvider& heightProvider, const WFMath::Point<3>& point) :
			mEntity(entity), mMarkerEntity(0), mMarkerNode(0), mMarkerDirectionIndicator(0), mHeightProvider(heightProvider), mPoint(point)
	{
		mMarkerNode = sceneManager.getRootSceneNode()->createChildSceneNode();
		try {
			mMarkerEntity = sceneManager.createEntity("common/primitives/model/sphere.mesh");
			//start out with a normal material
			mMarkerEntity->setMaterialName("/common/base/authoring/point");
			//The material is made to ignore depth checks, so if we put it in a later queue we're
			//making sure that the marker is drawn on top of everything else, making it easier to interact with.
			mMarkerEntity->setRenderQueueGroup(Ogre::RENDER_QUEUE_9);
			mMarkerEntity->setRenderingDistance(300);
			mMarkerEntity->setQueryFlags(MousePicker::CM_NONPICKABLE);
			mMarkerNode->attachObject(mMarkerEntity);
		} catch (const std::exception& ex) {
			S_LOG_WARNING("Error when creating marker node." << ex);
			return;
		}
		mMarkerNode->setVisible(true);

		mMarkerDirectionIndicator = new ShapeVisual(*sceneManager.getRootSceneNode(), false);

		mEntity.Moved.connect(sigc::mem_fun(*this, &EntityPointMarker::entityMoved));
	}

	/**
	 * @brief Dtor.
	 */
	~EntityPointMarker()
	{
		if (mMarkerEntity) {
			mMarkerEntity->_getManager()->destroyEntity(mMarkerEntity);
		}
		if (mMarkerNode) {
			mMarkerNode->getCreator()->destroySceneNode(mMarkerNode);
		}

		delete mMarkerDirectionIndicator;
	}

};

EntityEditor::EntityEditor(World& world, Eris::Entity& entity, Adapters::Atlas::MapAdapter* rootAdapter) :
		mWorld(world), mRootAdapter(rootAdapter), mEntity(entity), mMarker(nullptr), mPathPolygon(nullptr), mHasPath(false)
{
	mEntity.Moved.connect(sigc::mem_fun(*this, &EntityEditor::entityMoved));
}

EntityEditor::~EntityEditor()
{
	delete mRootAdapter;

	delete mMarker;

	delete mPathPolygon;
}

void EntityEditor::submitChanges()
{
	if (mRootAdapter->hasChanges()) {
		Atlas::Message::Element rootElement = mRootAdapter->getSelectedChangedElements();
		if (rootElement.isMap()) {
			std::map<std::string, ::Atlas::Message::Element> attributes(rootElement.asMap());
			if (attributes.size()) {

				std::stringstream ss;

				Atlas::Message::QueuedDecoder decoder;

				Atlas::Codecs::XML codec(ss, ss, decoder);
				Atlas::Formatter formatter(ss, codec);
				Atlas::Message::Encoder encoder(formatter);
				formatter.streamBegin();
				encoder.streamMessageElement(attributes);
				formatter.streamEnd();
				S_LOG_VERBOSE("Sending attribute update to server:\n" << ss.str());

				EmberServices::getSingleton().getServerService().setAttributes(&mEntity, attributes);
			}
		}
	}
}

Atlas::Message::Element EntityEditor::createMapElement()
{
	return Element(MapType());
}

Atlas::Message::Element EntityEditor::createListElement()
{
	return Element(ListType());
}
Atlas::Message::Element EntityEditor::createStringElement()
{
	return Element("");
}
Atlas::Message::Element EntityEditor::createIntElement()
{
	return Element(0);
}
Atlas::Message::Element EntityEditor::createFloatElement()
{
	return Element(0.0f);
}
Atlas::Message::Element EntityEditor::createPosition2dElement()
{
	ListType list;
	list.push_back(createFloatElement());
	list.push_back(createFloatElement());
	return Element(list);
}

void EntityEditor::addGoal(const std::string& definition)
{
	Eris::Account* account = EmberServices::getSingleton().getServerService().getAccount();

	Atlas::Objects::Entity::Anonymous thought;
	thought->setAttr("goal", definition);
	thought->setId(definition);

	Atlas::Objects::Operation::Set setOp;
	setOp->setArgs1(thought);

	Atlas::Objects::Operation::RootOperation thinkOp;
	std::list<std::string> parents;
	parents.emplace_back("think");
	thinkOp->setArgs1(setOp);
	thinkOp->setParents(parents);
	thinkOp->setTo(mEntity.getId());
	//By setting it TO an entity and FROM our avatar we'll make the server deliver it as
	//if it came from the entity itself (the server rewrites the FROM to be of the entity).
	thinkOp->setFrom(mWorld.getAvatar()->getEmberEntity().getId());

	Eris::Connection* connection = account->getConnection();

	connection->send(thinkOp);

}
void EntityEditor::updateGoal(const std::string& replaceDefinition, const std::string& definition)
{
	Eris::Account* account = EmberServices::getSingleton().getServerService().getAccount();

	Atlas::Objects::Entity::Anonymous thought;
	thought->setAttr("goal", definition);
	thought->setId(replaceDefinition);

	Atlas::Objects::Operation::Set setOp;
	setOp->setArgs1(thought);

	Atlas::Objects::Operation::RootOperation thinkOp;
	std::list<std::string> parents;
	parents.emplace_back("think");
	thinkOp->setParents(parents);
	thinkOp->setArgs1(setOp);
	thinkOp->setTo(mEntity.getId());
	//By setting it TO an entity and FROM our avatar we'll make the server deliver it as
	//if it came from the entity itself (the server rewrites the FROM to be of the entity).
	thinkOp->setFrom(mWorld.getAvatar()->getEmberEntity().getId());

	Eris::Connection* connection = account->getConnection();

	connection->send(thinkOp);
}

void EntityEditor::removeGoal(const std::string& definition)
{
	Eris::Account* account = EmberServices::getSingleton().getServerService().getAccount();

	Atlas::Objects::Entity::Anonymous thought;
	thought->setAttr("id", definition);

	Atlas::Objects::Operation::Delete deleteOp;
	deleteOp->setArgs1(thought);

	Atlas::Objects::Operation::RootOperation thinkOp;
	thinkOp->setArgs1(deleteOp);
	std::list<std::string> parents;
	parents.emplace_back("think");
	thinkOp->setParents(parents);
	thinkOp->setTo(mEntity.getId());
	//By setting it TO an entity and FROM our avatar we'll make the server deliver it as
	//if it came from the entity itself (the server rewrites the FROM to be of the entity).
	thinkOp->setFrom(mWorld.getAvatar()->getEmberEntity().getId());

	Eris::Connection* connection = account->getConnection();

	connection->send(thinkOp);
}

void EntityEditor::addKnowledge(const std::string& predicate, const std::string& subject, const std::string& knowledge)
{
	Eris::Account* account = EmberServices::getSingleton().getServerService().getAccount();

	Atlas::Objects::Entity::Anonymous thought;
	thought->setAttr("predicate", predicate);
	thought->setAttr("subject", subject);
	thought->setAttr("object", knowledge);

	Atlas::Objects::Operation::Set setOp;
	setOp->setArgs1(thought);

	Atlas::Objects::Operation::RootOperation thinkOp;
	std::list<std::string> parents;
	parents.emplace_back("think");
	thinkOp->setParents(parents);
	thinkOp->setArgs1(setOp);
	thinkOp->setTo(mEntity.getId());
	//By setting it TO an entity and FROM our avatar we'll make the server deliver it as
	//if it came from the entity itself (the server rewrites the FROM to be of the entity).
	thinkOp->setFrom(mWorld.getAvatar()->getEmberEntity().getId());

	Eris::Connection* connection = account->getConnection();

	connection->send(thinkOp);

}

void EntityEditor::addMarker(const std::string& entityId, const WFMath::Point<3>& point)
{
	delete mMarker;
	mMarker = 0;
	if (point.isValid()) {

		Eris::Entity* entity = mWorld.getView().getEntity(entityId);
		if (entity) {
			const WFMath::Point<3> worldPosition = entity->getViewPosition() + WFMath::Vector<3>(point);
			delete mMarker;
			mMarker = new EntityPointMarker(mEntity, mWorld.getSceneManager(), mWorld.getTerrainManager(), worldPosition);
			mMarker->updateMarker();
		}
	}
}

void EntityEditor::getGoals()
{
	Eris::Account* account = EmberServices::getSingleton().getServerService().getAccount();

	Atlas::Objects::Operation::RootOperation thinkOp;
	std::list<std::string> parents;
	parents.emplace_back("think");
	thinkOp->setParents(parents);
	thinkOp->setTo(mEntity.getId());

	Atlas::Objects::Operation::Get getOp;
	Atlas::Objects::Entity::Anonymous get_args;
	get_args->setAttr("goal", Atlas::Message::MapType());
	getOp->setArgs1(get_args);

	thinkOp->setArgs1(getOp);

	//By setting it TO an entity and FROM our avatar we'll make the server deliver it as
	//if it came from the entity itself (the server rewrites the FROM to be of the entity).
	thinkOp->setFrom(mWorld.getAvatar()->getEmberEntity().getId());
	//By setting a serial number we tell the server to "relay" the operation. This means that any
	//response operation from the target entity will be sent back to us.
	thinkOp->setSerialno(Eris::getNewSerialno());

	Eris::Connection* connection = account->getConnection();

	connection->getResponder()->await(thinkOp->getSerialno(), this, &EntityEditor::operationGetGoalsResult);
	connection->send(thinkOp);

}

void EntityEditor::getPath()
{
	Eris::Account* account = EmberServices::getSingleton().getServerService().getAccount();

	Atlas::Objects::Operation::RootOperation thinkOp;
	std::list<std::string> parents;
	parents.emplace_back("think");
	thinkOp->setParents(parents);
	thinkOp->setTo(mEntity.getId());

	Atlas::Objects::Operation::Get getOp;
	Atlas::Objects::Entity::Anonymous get_args;
	get_args->setAttr("path", Atlas::Message::MapType());
	getOp->setArgs1(get_args);

	thinkOp->setArgs1(getOp);

	//By setting it TO an entity and FROM our avatar we'll make the server deliver it as
	//if it came from the entity itself (the server rewrites the FROM to be of the entity).
	thinkOp->setFrom(mWorld.getAvatar()->getEmberEntity().getId());
	//By setting a serial number we tell the server to "relay" the operation. This means that any
	//response operation from the target entity will be sent back to us.
	thinkOp->setSerialno(Eris::getNewSerialno());

	Eris::Connection* connection = account->getConnection();

	connection->getResponder()->await(thinkOp->getSerialno(), this, &EntityEditor::operationGetPathResult);
	connection->send(thinkOp);

}

void EntityEditor::getThoughts()
{
	Eris::Account* account = EmberServices::getSingleton().getServerService().getAccount();

	Atlas::Objects::Operation::RootOperation thinkOp;
	std::list<std::string> parents;
	parents.emplace_back("think");
	thinkOp->setParents(parents);
	thinkOp->setTo(mEntity.getId());

	Atlas::Objects::Operation::Get getOp;
	thinkOp->setArgs1(getOp);

	//By setting it TO an entity and FROM our avatar we'll make the server deliver it as
	//if it came from the entity itself (the server rewrites the FROM to be of the entity).
	thinkOp->setFrom(mWorld.getAvatar()->getEmberEntity().getId());
	//By setting a serial number we tell the server to "relay" the operation. This means that any
	//response operation from the target entity will be sent back to us.
	thinkOp->setSerialno(Eris::getNewSerialno());

	Eris::Connection* connection = account->getConnection();

	connection->getResponder()->await(thinkOp->getSerialno(), this, &EntityEditor::operationGetThoughtResult);
	connection->send(thinkOp);

}

void EntityEditor::operationGetGoalsResult(const Atlas::Objects::Operation::RootOperation& op)
{
	//What we receive here has been relayed from the mind of the entity. That means that this op
	//is potentially unsafe, as it could be of any type (Set, Logout etc.), all depending on what the
	//mind client decided to send (i.e. someone might want to try to hack). We should therefore treat it
	//very carefully.

	if (op->getClassNo() == Atlas::Objects::Operation::ROOT_OPERATION_NO) {
		//An empty root operation signals a timeout; we never got any answer from the entity.
		return;
	}

	//We'll be getting back a Think op, which wraps a Set op, where the arguments are the thoughts.
	if (op->getParents().empty()) {
		S_LOG_WARNING("Got think operation without any parent set.");
		return;
	}
	if (op->getParents().front() != "think") {
		S_LOG_WARNING("Got think operation with wrong type set.");
		return;
	}

	if (op->getArgs().empty()) {
		S_LOG_WARNING("Got Thought op without any arguments.");
		return;
	}

	auto innerOp = op->getArgs().front();
	if (innerOp->getClassNo() != Atlas::Objects::Operation::SET_NO) {
		S_LOG_WARNING("Get Thought op with inner op that wasn't Set.");
	}

	auto setOp = Atlas::Objects::smart_dynamic_cast<Atlas::Objects::Operation::Set>(innerOp);

	if (!setOp->getArgs().empty()) {
		auto thoughts = setOp->getArgsAsList();
		for (auto thought : thoughts) {
			EventGotGoal(thought);
		}
	} else {
		EventGotEmptyGoals();
	}
}

void EntityEditor::operationGetPathResult(const Atlas::Objects::Operation::RootOperation& op)
{
	//What we receive here has been relayed from the mind of the entity. That means that this op
	//is potentially unsafe, as it could be of any type (Set, Logout etc.), all depending on what the
	//mind client decided to send (i.e. someone might want to try to hack). We should therefore treat it
	//very carefully.

	if (op->getClassNo() == Atlas::Objects::Operation::ROOT_OPERATION_NO) {
		//An empty root operation signals a timeout; we never got any answer from the entity.
		return;
	}

	//We'll be getting back a Think op, which wraps an anonymous op, where the arguments is the path.
	if (op->getParents().empty()) {
		S_LOG_WARNING("Got think operation without any parent set.");
		return;
	}
	if (op->getParents().front() != "think") {
		S_LOG_WARNING("Got think operation with wrong type set.");
		return;
	}

	if (op->getArgs().empty()) {
		S_LOG_WARNING("Got Thought op without any arguments.");
		return;
	}

	auto innerOp = op->getArgs().front();
	if (innerOp->getClassNo() != Atlas::Objects::Entity::ANONYMOUS_NO) {
		S_LOG_WARNING("Get Thought op with inner entity that wasn't anonymous.");
	}

	Atlas::Objects::Entity::Anonymous pathEntity = Atlas::Objects::smart_dynamic_cast<Atlas::Objects::Entity::Anonymous>(innerOp);

	if (!mPathPolygon) {
		mPathPolygon = new Authoring::Polygon(mWorld.getSceneManager().getRootSceneNode(), nullptr, false);
	}

	mPathPolygon->clear();

	std::vector<WFMath::Point<3>> points;
	WFMath::Polygon<2> poly;
	Element pathElem;
	if (pathEntity->copyAttr("path", pathElem) == 0) {
		mHasPath = true;
		if (pathElem.isList()) {
			const auto& path = pathElem.List();
			if (!path.empty()) {
				//Put one point at the entity itself.
				auto polygonPoint = mPathPolygon->appendPoint();
				polygonPoint->setLocalPosition(mEntity.getPredictedPos());
				//Don't show the ball since it will be over the entity itself.
				polygonPoint->setVisible(false);

				for (auto pathPoint : path) {
					if (pathPoint.isList()) {
						auto& list = pathPoint.List();
						if (list.size() == 3) {
							if (list[0].isFloat() && list[1].isFloat() && list[2].isFloat()) {
								WFMath::Point<3> point(list[0].Float(), list[1].Float(), list[2].Float());
								mPathPolygon->appendPoint()->setLocalPosition(point);
							}
						}
					}
				}

			}
		}
	}
	mPathPolygon->updateRender();
}


void EntityEditor::entityMoved()
{
	if (mPathPolygon && !mPathPolygon->getPoints().empty()) {
		mPathPolygon->getPoints().front()->setLocalPosition(mEntity.getPredictedPos());
		mPathPolygon->updateRender();
	}
	//If the entity has the ability to send back a path we should update it when the entity moves.
	if (mHasPath) {
		getPath();
	}

}


void EntityEditor::operationGetThoughtResult(const Atlas::Objects::Operation::RootOperation& op)
{
	//What we receive here has been relayed from the mind of the entity. That means that this op
	//is potentially unsafe, as it could be of any type (Set, Logout etc.), all depending on what the
	//mind client decided to send (i.e. someone might want to try to hack). We should therefore treat it
	//very carefully.

	if (op->getClassNo() == Atlas::Objects::Operation::ROOT_OPERATION_NO) {
		//An empty root operation signals a timeout; we never got any answer from the entity.
		return;
	}

	//We'll be getting back a Think op, which wraps a Set op, where the arguments are the thoughts.
	if (op->getParents().empty()) {
		S_LOG_WARNING("Got think operation without any parent set.");
		return;
	}
	if (op->getParents().front() != "think") {
		S_LOG_WARNING("Got think operation with wrong type set.");
		return;
	}

	if (op->getArgs().empty()) {
		S_LOG_WARNING("Got Thought op without any arguments.");
		return;
	}

	auto innerOp = op->getArgs().front();
	if (innerOp->getClassNo() != Atlas::Objects::Operation::SET_NO) {
		S_LOG_WARNING("Get Thought op with inner op that wasn't Set.");
	}

	auto setOp = Atlas::Objects::smart_dynamic_cast<Atlas::Objects::Operation::Set>(innerOp);

	if (!setOp->getArgs().empty()) {
		auto thoughts = setOp->getArgsAsList();
		for (auto thought : thoughts) {
			EventGotThought(thought);
		}
	} else {
		S_LOG_VERBOSE("Got thought op without any thoughts.");
	}
}

void EntityEditor::getGoalInfo(const std::string& definition)
{

	Eris::Account* account = EmberServices::getSingleton().getServerService().getAccount();

	Atlas::Objects::Operation::RootOperation thinkOp;
	std::list<std::string> parents;
	parents.emplace_back("think");
	thinkOp->setParents(parents);
	thinkOp->setTo(mEntity.getId());

	Atlas::Objects::Operation::Look lookOp;
	Atlas::Objects::Entity::Anonymous look_args;
	look_args->setAttr("id", definition);
	lookOp->setArgs1(look_args);

	thinkOp->setArgs1(lookOp);

	//By setting it TO an entity and FROM our avatar we'll make the server deliver it as
	//if it came from the entity itself (the server rewrites the FROM to be of the entity).
	thinkOp->setFrom(mWorld.getAvatar()->getEmberEntity().getId());
	//By setting a serial number we tell the server to "relay" the operation. This means that any
	//response operation from the target entity will be sent back to us.
	thinkOp->setSerialno(Eris::getNewSerialno());

	Eris::Connection* connection = account->getConnection();

	connection->getResponder()->await(thinkOp->getSerialno(), this, &EntityEditor::operationGetGoalInfoResult);
	connection->send(thinkOp);
}

void EntityEditor::operationGetGoalInfoResult(const Atlas::Objects::Operation::RootOperation& op)
{
	//What we receive here has been relayed from the mind of the entity. That means that this op
	//is potentially unsafe, as it could be of any type (Set, Logout etc.), all depending on what the
	//mind client decided to send (i.e. someone might want to try to hack). We should therefore treat it
	//very carefully.

	if (op->getClassNo() == Atlas::Objects::Operation::ROOT_OPERATION_NO) {
		//An empty root operation signals a timeout; we never got any answer from the entity.
		return;
	}

	//Since we'll just be iterating over the args we only need to do an extra check that what we got is a
	//"info" operation.
	if (op->getParents().empty() || op->getParents().front() != "think") {
		S_LOG_WARNING("Got goal info operation with wrong type.");
		return;
	}

	if (op->getArgs().empty()) {
		S_LOG_WARNING("Got Thought op without any arguments.");
		return;
	}

	auto innerOp = op->getArgs().front();
	if (innerOp->getClassNo() != Atlas::Objects::Operation::INFO_NO) {
		S_LOG_WARNING("Get Thought op with inner op that wasn't Info.");
	}

	auto infoOp = Atlas::Objects::smart_dynamic_cast<Atlas::Objects::Operation::Info>(innerOp);

	if (!infoOp->getArgs().empty()) {
		auto goalInfos = infoOp->getArgsAsList();
		for (auto goalInfo : goalInfos) {
			EventGotGoalInfo(goalInfo);
		}
	} else {
		S_LOG_VERBOSE("Got goal info op without any goals.");
	}
}

std::string EntityEditor::parseElementMap(const Atlas::Message::MapType& map)
{
	std::stringstream ss;

	Atlas::PresentationBridge bridge(ss);
	Atlas::Message::Encoder encoder(bridge);
	encoder.streamMessageElement(map);

	return ss.str();
}

void EntityEditor::removeMarker()
{
	delete mMarker;
	mMarker = 0;
}

WFMath::Point<3> EntityEditor::createPoint(float x, float y, float z)
{
	return WFMath::Point<3>(x, y, z);
}

}

}
}
